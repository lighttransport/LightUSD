// Shared usdzconvert orchestration used by both the browser module
// (web/js/usdzconvert.js) and the Node CLI (web/js/cli/usdzconvert.js).
//
// Flow: load the root USD as a layer -> for every referenced texture, optionally
// resize / re-encode it (via an injected browser texture processor, or the WASM
// `convertImage` binding as fallback) and register it in the asset cache under
// its USD-relative name -> exportAsUSDZ() (which flattens the stage and packs
// the cached image assets).

import { bakeUDIMAtlas, bakeLayerUDIM, udimOptions } from './udim-bake.js';

import { normalizeTextureConcurrency } from './texture-memory-budget.mjs';

const USD_RE = /\.(usd|usda|usdc|usdz)$/i;
const IMG_RE = /\.(png|jpg|jpeg|exr|hdr|ktx2|bmp|tga|tiff?|webp|qoi|avif)$/i;
// Audio formats referenced by UsdMediaSpatialAudio (filePath asset). USD itself
// is codec-agnostic; these are the formats commonly embedded in USDZ.
const AUDIO_RE = /\.(m4a|mp3|wav|aac|ogg|flac|aiff|aif)$/i;

export function isImageName(name) {
  return IMG_RE.test(name);
}

export function isUsdName(name) {
  return USD_RE.test(name);
}

export function isAudioName(name) {
  return AUDIO_RE.test(name);
}

// Format string for the WASM convertImage() encoder, or null if we should not
// re-encode (e.g. AVIF — left untouched). EXR is encodable now (recent USDZ
// allows EXR textures): the WASM build can resize and re-encode it, or transcode
// it to PNG/JPEG.
export function imageFormatFromName(name) {
  const ext = (name.toLowerCase().split('.').pop() || '');
  if (ext === 'jpg' || ext === 'jpeg') return 'jpeg';
  if (ext === 'png') return 'png';
  if (ext === 'exr' || ext === 'hdr') return 'exr';
  if (['ktx2','bmp','tga','tif','tiff','webp','qoi'].includes(ext))return 'png';
  return null;
}

export function normalizedTextureFormat(format) {
  const f = String(format || 'keep').toLowerCase();
  if (f === 'jpg') return 'jpeg';
  if (f === 'jpeg' || f === 'png' || f === 'exr') return f;
  return 'keep';
}

export function outputFormatForImage(name, textureFormat = 'keep') {
  const requested = normalizedTextureFormat(textureFormat);
  const fmt = requested === 'keep' ? imageFormatFromName(name) : requested;
  if (fmt === 'jpeg') {
    const originalExt = (name.toLowerCase().split('.').pop() || '');
    const ext = requested === 'keep' && originalExt === 'jpeg' ? 'jpeg' : 'jpg';
    return { format: 'jpeg', ext };
  }
  if (fmt === 'png') return { format: 'png', ext: 'png' };
  // Keep EXR as EXR (resize-only), or force EXR output for any input.
  if (fmt === 'exr') return { format: 'exr', ext: 'exr' };
  return { format: null, ext: null };
}

const ZIP_LOCAL_HEADER_SIZE = 30;
const ZIP_CENTRAL_DIR_HEADER_SIZE = 46;
const USDZ_ALIGNMENT = 64;

function crc32Table() {
  if (crc32Table.cache) return crc32Table.cache;
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) {
      c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    table[i] = c >>> 0;
  }
  crc32Table.cache = table;
  return table;
}

function crc32(bytes) {
  const table = crc32Table();
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    c = table[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function zipPaddingForDataOffset(offset, nameLength) {
  const headerSize = ZIP_LOCAL_HEADER_SIZE + nameLength;
  const remainder = (offset + headerSize) % USDZ_ALIGNMENT;
  return remainder === 0 ? 0 : USDZ_ALIGNMENT - remainder;
}

function assertZip32Size(name, size) {
  if (size > 0xffffffff) {
    throw new Error(`USDZ entry "${name}" exceeds ZIP32 size limit.`);
  }
}

function writeAsciiName(out, offset, nameBytes) {
  out.set(nameBytes, offset);
  return offset + nameBytes.length;
}

function writeLocalEntry(out, offset, entry) {
  const dv = new DataView(out.buffer, out.byteOffset, out.byteLength);
  const nameBytes = entry.nameBytes;
  const data = entry.data;
  const padding = zipPaddingForDataOffset(offset, nameBytes.length);
  if (padding > 0xffff) {
    throw new Error(`USDZ alignment padding is too large for "${entry.name}".`);
  }
  entry.localHeaderOffset = offset;

  dv.setUint32(offset + 0, 0x04034b50, true);
  dv.setUint16(offset + 4, 20, true);
  dv.setUint16(offset + 6, 0, true);
  dv.setUint16(offset + 8, 0, true);
  dv.setUint16(offset + 10, 0, true);
  dv.setUint16(offset + 12, 0, true);
  dv.setUint32(offset + 14, entry.crc32 >>> 0, true);
  dv.setUint32(offset + 18, data.length >>> 0, true);
  dv.setUint32(offset + 22, data.length >>> 0, true);
  dv.setUint16(offset + 26, nameBytes.length, true);
  dv.setUint16(offset + 28, padding, true);
  offset += ZIP_LOCAL_HEADER_SIZE;
  offset = writeAsciiName(out, offset, nameBytes);
  offset += padding;
  if (offset % USDZ_ALIGNMENT !== 0) {
    throw new Error(`Internal error: USDZ alignment failed for "${entry.name}".`);
  }
  out.set(data, offset);
  return offset + data.length;
}

function writeCentralDirectory(out, offset, entries) {
  const dv = new DataView(out.buffer, out.byteOffset, out.byteLength);
  const cdOffset = offset;
  for (const entry of entries) {
    const nameBytes = entry.nameBytes;
    dv.setUint32(offset + 0, 0x02014b50, true);
    dv.setUint16(offset + 4, 20, true);
    dv.setUint16(offset + 6, 20, true);
    dv.setUint16(offset + 8, 0, true);
    dv.setUint16(offset + 10, 0, true);
    dv.setUint16(offset + 12, 0, true);
    dv.setUint16(offset + 14, 0, true);
    dv.setUint32(offset + 16, entry.crc32 >>> 0, true);
    // Use the stored size: entry.data may have been freed after its local entry
    // was written (consume mode) to bound peak memory.
    const entrySize = (entry.size != null ? entry.size : entry.data.length) >>> 0;
    dv.setUint32(offset + 20, entrySize, true);
    dv.setUint32(offset + 24, entrySize, true);
    dv.setUint16(offset + 28, nameBytes.length, true);
    dv.setUint16(offset + 30, 0, true);
    dv.setUint16(offset + 32, 0, true);
    dv.setUint16(offset + 34, 0, true);
    dv.setUint16(offset + 36, 0, true);
    dv.setUint32(offset + 38, 0, true);
    dv.setUint32(offset + 42, entry.localHeaderOffset >>> 0, true);
    offset += ZIP_CENTRAL_DIR_HEADER_SIZE;
    offset = writeAsciiName(out, offset, nameBytes);
  }
  const cdSize = offset - cdOffset;
  if (entries.length > 0xffff || cdOffset > 0xffffffff || cdSize > 0xffffffff) {
    throw new Error('USDZ central directory exceeds ZIP32 limits.');
  }

  dv.setUint32(offset + 0, 0x06054b50, true);
  dv.setUint16(offset + 4, 0, true);
  dv.setUint16(offset + 6, 0, true);
  dv.setUint16(offset + 8, entries.length, true);
  dv.setUint16(offset + 10, entries.length, true);
  dv.setUint32(offset + 12, cdSize >>> 0, true);
  dv.setUint32(offset + 16, cdOffset >>> 0, true);
  dv.setUint16(offset + 20, 0, true);
  return offset + 22;
}

// Parse a STORE-only USDZ/ZIP archive into ordered entry descriptors. Entry
// `data` fields are subarray views into the original archive bytes.
export function parseUSDZEntries(bytes) {
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (u8.length < 22) throw new Error('Not a valid USDZ/ZIP (too small).');
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const td = new TextDecoder();

  // Work is shared across candidates: forged comment records must not cause
  // unbounded reparsing of a large directory.
  let work = Math.max(65536, Math.ceil(u8.length / ZIP_CENTRAL_DIR_HEADER_SIZE) * 4);
  const readDirectory = (eocd) => {
  const count = dv.getUint16(eocd + 10, true);
  if (dv.getUint16(eocd + 4, true) !== 0 ||
      dv.getUint16(eocd + 6, true) !== 0 ||
      dv.getUint16(eocd + 8, true) !== count) {
    throw new Error('Unsupported USDZ: multi-disk ZIP archive.');
  }
  const centralStart = dv.getUint32(eocd + 16, true);
  const centralSize = dv.getUint32(eocd + 12, true);
  const centralEnd = centralStart + centralSize;
  if (centralEnd !== eocd || centralSize < count * ZIP_CENTRAL_DIR_HEADER_SIZE) {
    throw new Error('Corrupt USDZ: invalid central directory range.');
  }
  let off = centralStart;
  const entries = [];
  const names = new Set();
  const ranges = [];
  for (let n = 0; n < count; n++) {
    if (--work < 0) throw new Error('Corrupt USDZ: EOCD validation work limit exceeded.');
    if (off + ZIP_CENTRAL_DIR_HEADER_SIZE > centralEnd ||
        dv.getUint32(off, true) !== 0x02014b50) {
      throw new Error('Corrupt USDZ: bad central directory header.');
    }
    const flags = dv.getUint16(off + 8, true);
    const method = dv.getUint16(off + 10, true);
    const crc = dv.getUint32(off + 16, true);
    const compSize = dv.getUint32(off + 20, true);
    const uncompSize = dv.getUint32(off + 24, true);
    const nameLen = dv.getUint16(off + 28, true);
    const extraLen = dv.getUint16(off + 30, true);
    const commentLen = dv.getUint16(off + 32, true);
    const lho = dv.getUint32(off + 42, true);
    if (off + ZIP_CENTRAL_DIR_HEADER_SIZE + nameLen + extraLen + commentLen >
        centralEnd) {
      throw new Error('Corrupt USDZ: truncated central directory entry.');
    }
    const name = td.decode(u8.subarray(off + 46, off + 46 + nameLen));
    if (!name || names.has(name)) {
      throw new Error(`Corrupt USDZ: empty or duplicate entry name "${name}".`);
    }
    names.add(name);
    if (dv.getUint16(off + 34, true) !== 0 || (flags & 0x41) !== 0) {
      throw new Error(`Unsupported USDZ: encrypted or multi-disk entry "${name}".`);
    }

    if (name.endsWith('/')) throw new Error('Unsupported USDZ: directory entry.');
    {
      if (method !== 0) {
        throw new Error(`USDZ entry "${name}" is compressed (method ${method}); ` +
          'only STORE is supported.');
      }
      if (compSize !== uncompSize) {
        throw new Error(`USDZ entry "${name}" has mismatched compressed/uncompressed size.`);
      }
      if (lho + ZIP_LOCAL_HEADER_SIZE > centralStart ||
          dv.getUint32(lho, true) !== 0x04034b50) {
        throw new Error(`Corrupt USDZ: bad local header for "${name}".`);
      }
      const lNameLen = dv.getUint16(lho + 26, true);
      const lExtraLen = dv.getUint16(lho + 28, true);
      const dataStart = lho + ZIP_LOCAL_HEADER_SIZE + lNameLen + lExtraLen;
      const dataEnd = dataStart + compSize;
      if (dataEnd > centralStart) {
        throw new Error(`Corrupt USDZ: truncated data for "${name}".`);
      }
      if (dv.getUint16(lho + 6, true) !== flags ||
          dv.getUint16(lho + 8, true) !== method || lNameLen !== nameLen ||
          u8.subarray(lho + 30, lho + 30 + lNameLen).some(
            (byte, i) => byte !== u8[off + 46 + i]) ||
          (!(flags & 8) && (dv.getUint32(lho + 14, true) !== crc ||
            dv.getUint32(lho + 18, true) !== compSize ||
            dv.getUint32(lho + 22, true) !== uncompSize))) {
        throw new Error(`Corrupt USDZ: local header disagrees with directory for "${name}".`);
      }
      if (dataStart % USDZ_ALIGNMENT !== 0) {
        throw new Error(`USDZ entry "${name}" is not 64-byte aligned.`);
      }
      entries.push({
        name,
        data: u8.subarray(dataStart, dataEnd),
        size: compSize,
        crc32: crc >>> 0,
        index: n,
        localHeaderOffset: lho,
      });
      let recordEnd = dataEnd;
      if (flags & 8) {
        const signed = recordEnd + 4 <= centralStart && dv.getUint32(recordEnd, true) === 0x08074b50;
        const descriptor = recordEnd + (signed ? 4 : 0);
        if (descriptor + 12 > centralStart || dv.getUint32(descriptor, true) !== crc ||
            dv.getUint32(descriptor + 4, true) !== compSize ||
            dv.getUint32(descriptor + 8, true) !== uncompSize) {
          throw new Error(`Corrupt USDZ: bad data descriptor for "${name}".`);
        }
        recordEnd = descriptor + 12;
      }
      ranges.push([lho, recordEnd]);
    }
    off += ZIP_CENTRAL_DIR_HEADER_SIZE + nameLen + extraLen + commentLen;
  }
  if (off !== centralEnd) {
    throw new Error('Corrupt USDZ: central directory entry count mismatch.');
  }
  ranges.sort((a, b) => a[0] - b[0]);
  for (let i = 1; i < ranges.length; i++) {
    if (ranges[i][0] < ranges[i - 1][1]) {
      throw new Error('Corrupt USDZ: overlapping local entries.');
    }
  }
  if (ranges.length && (ranges[0][0] !== 0 || ranges[ranges.length - 1][1] !== centralStart ||
      ranges.some((range, i) => i > 0 && range[0] !== ranges[i - 1][1]))) {
    throw new Error('Corrupt USDZ: central directory omits physical entries or contains gaps.');
  }
  entries.sort((a, b) => a.localHeaderOffset - b.localHeaderOffset);
  if (!entries.length || entries[0].localHeaderOffset !== 0) {
    throw new Error('Corrupt USDZ: missing first local entry.');
  }
  return entries;
  };

  let candidateError = null;
  const minStart = Math.max(0, u8.length - 22 - 0xffff);
  for (let i = u8.length - 22; i >= minStart; i--) {
    if (dv.getUint32(i, true) !== 0x06054b50 ||
        i + 22 + dv.getUint16(i + 20, true) !== u8.length) continue;
    try { return readDirectory(i); }
    catch (error) {
      candidateError = error;
      if (work < 0) throw error;
    }
  }
  throw candidateError || new Error('Not a valid USDZ/ZIP (no EOCD record).');
}

// Match the native reader: the first physical entry is the package root,
// including extension-neutral .usd files. A later layer cannot replace it.
export function usdRootEntry(entries) {
  const root = entries[0];
  const data = root && root.data;
  const magic = data && new TextDecoder().decode(data.subarray(0, 8));
  const usda = magic && magic.startsWith('#usda');
  const usdc = magic === 'PXR-USDC';
  if (!root || !(/\.usda$/i.test(root.name) && usda ||
                 /\.usdc$/i.test(root.name) && usdc ||
                 /\.usd$/i.test(root.name) && (usda || usdc))) {
    throw new Error('Corrupt USDZ: first physical entry is not a valid USD root layer.');
  }
  return root;
}

export function buildUSDZWithNewRoot(rootName, rootData, passthroughEntries,
                                     opts = {}) {
  // consume: free each entry's bytes (and the caller's source reference) right
  // after writing its local entry, so the source arrays and the growing output
  // buffer do not both fully coexist. Lets texture-streaming repack a large
  // .usdz without holding 2× the output in memory. Safe only when the caller no
  // longer needs the passed-in entry/root bytes afterward.
  const consume = !!opts.consume;
  const encoder = new TextEncoder();
  const rootBytes = rootData instanceof Uint8Array ? rootData : new Uint8Array(rootData);
  const entries = [{
    name: rootName,
    nameBytes: encoder.encode(rootName),
    data: rootBytes,
    size: rootBytes.length,
    crc32: crc32(rootBytes),
    localHeaderOffset: 0,
    _src: null,
  }];
  for (const src of passthroughEntries) {
    const data = src.data instanceof Uint8Array ? src.data : new Uint8Array(src.data);
    entries.push({
      name: src.name,
      nameBytes: encoder.encode(src.name),
      data,
      size: data.length,
      crc32: src.crc32 >>> 0,
      localHeaderOffset: 0,
      _src: consume ? src : null,
    });
  }

  let offset = 0;
  for (const entry of entries) {
    assertZip32Size(entry.name, entry.data.length);
    if (entry.nameBytes.length > 0xffff) {
      throw new Error(`USDZ entry name is too long: "${entry.name}".`);
    }
    const padding = zipPaddingForDataOffset(offset, entry.nameBytes.length);
    offset += ZIP_LOCAL_HEADER_SIZE + entry.nameBytes.length + padding +
      entry.data.length;
    if (offset > 0xffffffff) {
      throw new Error('USDZ archive exceeds ZIP32 size limit.');
    }
  }
  const centralDirOffset = offset;
  for (const entry of entries) {
    offset += ZIP_CENTRAL_DIR_HEADER_SIZE + entry.nameBytes.length;
  }
  const centralDirSize = offset - centralDirOffset;
  if (entries.length > 0xffff || centralDirOffset > 0xffffffff ||
      centralDirSize > 0xffffffff || offset + 22 > 0xffffffff) {
    throw new Error('USDZ central directory exceeds ZIP32 limits.');
  }
  offset += 22;

  const out = new Uint8Array(offset);
  let writeOffset = 0;
  for (const entry of entries) {
    writeOffset = writeLocalEntry(out, writeOffset, entry);
    if (consume) {
      // Bytes are now copied into `out`; drop the source references so the
      // input/re-encoded buffers can be reclaimed while the rest is written.
      entry.data = null;
      if (entry._src) { entry._src.data = null; entry._src = null; }
    }
  }
  writeOffset = writeCentralDirectory(out, writeOffset, entries);
  if (writeOffset !== out.length) {
    throw new Error('Internal error: USDZ output size mismatch.');
  }
  return out;
}

// Incremental STORE-only USDZ writer. addEntry() emits one 64-byte-aligned local
// entry to `sink` immediately (so the caller can release the bytes right after),
// and finalize() appends the central directory + EOCD. Only one entry's bytes are
// held at a time, so a large texture-streamed archive can be written to disk
// without the whole output (and all its source entries) coexisting in memory.
export class ZipStreamWriter {
  constructor(sink) {
    // sink is either a function (bytes)=>void that appends to the output, or an
    // object { write(bytes), patch(pos, bytes) }. A patch-capable (seekable)
    // sink additionally enables addEntryStreaming() — the local header's CRC and
    // sizes are written as placeholders, then patched after the streamed data.
    if (typeof sink === 'function') { this._write = sink; this._patch = null; }
    else if (sink && typeof sink.write === 'function') {
      this._write = sink.write.bind(sink);
      this._patch = typeof sink.patch === 'function' ? sink.patch.bind(sink) : null;
    } else {
      throw new Error('ZipStreamWriter requires a write-capable sink.');
    }
    this.offset = 0;
    this.entries = [];  // { nameBytes, crc32, size, localHeaderOffset }
    this._enc = new TextEncoder();
  }
  _emit(bytes) { this._write(bytes); this.offset += bytes.length; }
  canStream() { return !!this._patch; }  // addEntryStreaming needs a seekable sink
  addEntry(name, data) {
    const u8 = data instanceof Uint8Array ? data : new Uint8Array(data);
    assertZip32Size(name, u8.length);
    const nameBytes = this._enc.encode(name);
    if (nameBytes.length > 0xffff) {
      throw new Error(`USDZ entry name is too long: "${name}".`);
    }
    const padding = zipPaddingForDataOffset(this.offset, nameBytes.length);
    if (padding > 0xffff) {
      throw new Error(`USDZ alignment padding is too large for "${name}".`);
    }
    const localHeaderOffset = this.offset;
    if (localHeaderOffset > 0xffffffff) {
      throw new Error('USDZ archive exceeds ZIP32 size limit.');
    }
    const crc = crc32(u8) >>> 0;
    const head = new Uint8Array(ZIP_LOCAL_HEADER_SIZE + nameBytes.length + padding);
    const dv = new DataView(head.buffer);
    dv.setUint32(0, 0x04034b50, true);
    dv.setUint16(4, 20, true);
    dv.setUint32(14, crc, true);
    dv.setUint32(18, u8.length >>> 0, true);
    dv.setUint32(22, u8.length >>> 0, true);
    dv.setUint16(26, nameBytes.length, true);
    dv.setUint16(28, padding, true);
    head.set(nameBytes, ZIP_LOCAL_HEADER_SIZE);
    this._emit(head);
    this._emit(u8);
    if (this.offset > 0xffffffff) {
      throw new Error('USDZ archive exceeds ZIP32 size limit.');
    }
    this.entries.push({ nameBytes, crc32: crc, size: u8.length, localHeaderOffset });
  }
  // Stream one entry whose bytes are produced incrementally by streamFn(emit),
  // where emit(chunk) appends a Uint8Array. Used to write a large root layer
  // straight from the WASM writer without ever holding it in JS. The local
  // header's CRC/size are emitted as placeholders then patched after the data,
  // so this requires a patch-capable (seekable) sink. Output is byte-identical
  // to addEntry() with the same bytes. `emit`-supplied chunks are consumed
  // synchronously (copied into the CRC + the sink), so transient WASM-heap views
  // are safe.
  addEntryStreaming(name, streamFn) {
    if (!this._patch) {
      throw new Error('addEntryStreaming requires a patch-capable sink');
    }
    const nameBytes = this._enc.encode(name);
    if (nameBytes.length > 0xffff) {
      throw new Error(`USDZ entry name is too long: "${name}".`);
    }
    const padding = zipPaddingForDataOffset(this.offset, nameBytes.length);
    if (padding > 0xffff) {
      throw new Error(`USDZ alignment padding is too large for "${name}".`);
    }
    const localHeaderOffset = this.offset;
    if (localHeaderOffset > 0xffffffff) {
      throw new Error('USDZ archive exceeds ZIP32 size limit.');
    }
    // Local header with CRC/sizes left as 0 (patched after the data is streamed).
    const head = new Uint8Array(ZIP_LOCAL_HEADER_SIZE + nameBytes.length + padding);
    const dv = new DataView(head.buffer);
    dv.setUint32(0, 0x04034b50, true);
    dv.setUint16(4, 20, true);
    dv.setUint16(26, nameBytes.length, true);
    dv.setUint16(28, padding, true);
    head.set(nameBytes, ZIP_LOCAL_HEADER_SIZE);
    this._emit(head);

    const table = crc32Table();
    let crc = 0xffffffff;
    let size = 0;
    streamFn((chunk) => {
      const c = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
      for (let i = 0; i < c.length; i++) {
        crc = table[(crc ^ c[i]) & 0xff] ^ (crc >>> 8);
      }
      size += c.length;
      if (size > 0xffffffff) {
        throw new Error(`USDZ entry "${name}" exceeds ZIP32 size limit.`);
      }
      this._emit(c);  // copies synchronously (fs write / append)
    });
    crc = (crc ^ 0xffffffff) >>> 0;
    assertZip32Size(name, size);
    if (this.offset > 0xffffffff) {
      throw new Error('USDZ archive exceeds ZIP32 size limit.');
    }
    // Patch CRC32 (+14), compressed size (+18), uncompressed size (+22).
    const patch = new Uint8Array(12);
    const pdv = new DataView(patch.buffer);
    pdv.setUint32(0, crc, true);
    pdv.setUint32(4, size >>> 0, true);
    pdv.setUint32(8, size >>> 0, true);
    this._patch(localHeaderOffset + 14, patch);

    this.entries.push({ nameBytes, crc32: crc, size, localHeaderOffset });
  }
  finalize() {
    const cdOffset = this.offset;
    for (const e of this.entries) {
      const hdr = new Uint8Array(ZIP_CENTRAL_DIR_HEADER_SIZE + e.nameBytes.length);
      const dv = new DataView(hdr.buffer);
      dv.setUint32(0, 0x02014b50, true);
      dv.setUint16(4, 20, true);
      dv.setUint16(6, 20, true);
      dv.setUint32(16, e.crc32 >>> 0, true);
      dv.setUint32(20, e.size >>> 0, true);
      dv.setUint32(24, e.size >>> 0, true);
      dv.setUint16(28, e.nameBytes.length, true);
      dv.setUint32(42, e.localHeaderOffset >>> 0, true);
      hdr.set(e.nameBytes, ZIP_CENTRAL_DIR_HEADER_SIZE);
      this._emit(hdr);
    }
    const cdSize = this.offset - cdOffset;
    if (this.entries.length > 0xffff || cdOffset > 0xffffffff ||
        cdSize > 0xffffffff) {
      throw new Error('USDZ central directory exceeds ZIP32 limits.');
    }
    const eocd = new Uint8Array(22);
    const dv = new DataView(eocd.buffer);
    dv.setUint32(0, 0x06054b50, true);
    dv.setUint16(8, this.entries.length, true);
    dv.setUint16(10, this.entries.length, true);
    dv.setUint32(12, cdSize >>> 0, true);
    dv.setUint32(16, cdOffset >>> 0, true);
    this._emit(eocd);
  }
}

// Repack a flattened root + the archive's non-root entries into a USDZ. Images
// are re-encoded/resized one at a time (keep format only) when requested, else
// passed through. If opts.zipSink is given the archive is streamed straight to
// it (one entry held at a time — the lowest-peak path); otherwise it is built in
// memory and returned. Shared by the legacy texture-streaming path and the core
// low-memory pipeline. Returns { usdz, streamedToSink, textures, resized,
// reencoded, audio, otherAssets }.
// Build a per-texture resize-colorspace picker(name)->('srgb'|'linear'|'').
// With resizeColorspace:'auto' it reads each UsdUVTexture's authored
// sourceColorSpace from the root layer (basename-keyed): 'sRGB' -> 'srgb'
// (linear-light resample), everything else -> 'linear' (gamma-space, safe for
// data maps). Non-'auto' returns the global value for every texture.
function makeResizeCsPicker(native, rootBytes, opts) {
  const want = String((opts && opts.resizeColorspace) || '');
  if (want.toLowerCase() !== 'auto') return () => want;
  let csMap = null;
  if ((opts.maxTextureSize || 0) > 0 && rootBytes &&
      typeof native.getTextureColorspaceMap === 'function') {
    try { csMap = native.getTextureColorspaceMap(rootBytes); } catch (e) { csMap = null; }
  }
  const base = (n) => { const s = n.lastIndexOf('/'); return s < 0 ? n : n.slice(s + 1); };
  return (name) => {
    const cs = csMap ? (csMap[name] ?? (native.getTextureColorspaceMap.fullPaths ? undefined : csMap[base(name)])) : undefined;
    return (cs === 'sRGB' || cs === 'srgb') ? 'srgb' : 'linear';
  };
}

function repackUSDZEntries(native, rootName, rootData, archiveEntries, rootEntry,
                           opts, log) {
  const wantResize = (opts.maxTextureSize || 0) > 0;
  const keepFmt = normalizedTextureFormat(opts.textureFormat) === 'keep';
  const textureFormat = normalizedTextureFormat(opts.textureFormat);
  // Re-compressing a JPEG (decode->encode) is LOSSY and would decode the whole
  // image into the WASM heap; with no resize and no explicit quality reduction
  // it gains nothing, so JPEGs pass through losslessly. (PNG re-encodes via the
  // streaming transcoder in convertImage.) Only a resize or an explicit lower
  // --jpeg-quality re-encodes a JPEG.
  const jpegRecompress = (opts.jpegQuality || 90) < 90;
  // Role-aware resize colorspace: with resizeColorspace:'auto', read each
  // texture's authored UsdUVTexture sourceColorSpace from the root layer once.
  // 'sRGB' textures resample in linear light; everything else (raw/auto/unknown)
  // stays gamma-space — safe for linear data maps (normal/ORM/height).
  const roleAware = String(opts.resizeColorspace || '').toLowerCase() === 'auto';
  const resizeCsFor = makeResizeCsPicker(native, rootData, opts);
  let reencoded = 0, resized = 0, textures = 0, audio = 0, otherAssets = 0;
  const tally = (name) => {
    if (isImageName(name)) textures++;
    else if (isAudioName(name)) audio++;
    else if (!isUsdName(name)) otherAssets++;
  };
  const outputNameFor = (name) => {
    if (!isImageName(name)) return name;
    const fmtInfo = outputFormatForImage(name, textureFormat);
    return fmtInfo.format && fmtInfo.ext ? replaceExt(name, fmtInfo.ext) : name;
  };
  const produce = (e) => {
    if (isImageName(e.name)) {
      const fmtInfo = outputFormatForImage(e.name, textureFormat);
      const wantTextureWork = fmtInfo.format &&
        (wantResize || opts.reencode === true || !keepFmt);
      if (!wantTextureWork) return e.data;
      if (fmtInfo.format === 'jpeg' && imageFormatFromName(e.name) === 'jpeg' &&
          !wantResize && !jpegRecompress) {
        log(`  ${e.name}: ${e.data.length} bytes [jpeg passthrough — lossless]`);
        return e.data;
      }
      if (fmtInfo.format) {
        const res = native.convertImage(e.data, {
          maxSize: opts.maxTextureSize || 0,
          format: fmtInfo.format,
          pngEncoder: opts.pngEncoder || 'auto',
          jpegQuality: opts.jpegQuality || 90,
          resizeColorspace: resizeCsFor(e.name),
          // This is the single-usdz low-heap repack path: keep the
          // scanline-streamed PNG transcoder (slower, but the whole image
          // never lands in the wasm heap).
          lowMemory: 1,
        });
        if (res && res.success) {
          reencoded++;
          if (res.resized) resized++;
          const outName = outputNameFor(e.name);
          log(`  ${e.name}: ${e.data.length} -> ${res.data.length} bytes` +
              (outName !== e.name ? ` as ${outName}` : '') +
              (res.resized ? ` [resized ${res.width}x${res.height}` +
                 (roleAware ? ` ${resizeCsFor(e.name)}]` : ']') : ' [reencoded]'));
          return new Uint8Array(res.data);
        }
        log(`  ${e.name}: convertImage failed (${res && res.error}); passthrough`);
      }
    }
    return e.data;
  };

  // opts.zipSink may be a function (append-only) or an object { write, patch }.
  // rootData may be a Uint8Array, or { stream: fn } to stream the root entry
  // straight from the WASM writer (requires a patch-capable sink).
  const sink = opts.zipSink || null;
  let usdz = null;
  if (sink) {
    const zw = new ZipStreamWriter(sink);
    if (rootData && typeof rootData === 'object' && typeof rootData.stream === 'function') {
      zw.addEntryStreaming(rootName, rootData.stream);
    } else {
      zw.addEntry(rootName, rootData);
    }
    rootData = null;
    for (const e of archiveEntries) {
      if (e === rootEntry) continue;
      let data = produce(e);
      const outName = outputNameFor(e.name);
      zw.addEntry(outName, data);
      tally(outName);
      data = null;
      e.data = null;  // release the input slice reference as we go
    }
    zw.finalize();
  } else {
    const outEntries = [];
    for (const e of archiveEntries) {
      if (e === rootEntry) continue;
      const outName = outputNameFor(e.name);
      outEntries.push({ name: outName, data: produce(e) });
      tally(outName);
    }
    usdz = buildUSDZWithNewRoot(rootName, rootData, outEntries, { consume: true });
  }
  return { usdz, streamedToSink: !!sink, textures, resized, reencoded, audio,
           otherAssets };
}

function textureAssetPathRemapForEntries(entries, textureFormat) {
  const remap = {};
  const fmt = normalizedTextureFormat(textureFormat);
  if (fmt === 'keep') return remap;
  for (const e of entries) {
    if (!isImageName(e.name)) continue;
    const fmtInfo = outputFormatForImage(e.name, fmt);
    if (!fmtInfo.format || !fmtInfo.ext) continue;
    const outName = replaceExt(e.name, fmtInfo.ext);
    if (outName !== e.name) addTextureRemapAliases(remap, e.name, outName);
  }
  return remap;
}

function addTextureRemapAliases(remap, name, outName) {
  if (!remap || !name || !outName || name === outName) return;
  const normName = String(name).replace(/\\/g, '/').replace(/^\.\/+/, '').replace(/^\/+/, '');
  const normOut = String(outName).replace(/\\/g, '/').replace(/^\.\/+/, '').replace(/^\/+/, '');
  if (!normName || !normOut || normName === normOut) return;
  if (!Object.prototype.hasOwnProperty.call(remap, normName)) remap[normName] = normOut;

  const inParts = normName.split('/');
  const outParts = normOut.split('/');
  const n = Math.min(inParts.length, outParts.length);
  for (let i = 1; i < n; i++) {
    const inSuffix = inParts.slice(i).join('/');
    const outSuffix = outParts.slice(i).join('/');
    if (inSuffix && outSuffix && inSuffix !== outSuffix &&
        !Object.prototype.hasOwnProperty.call(remap, inSuffix)) {
      remap[inSuffix] = outSuffix;
    }
  }
}

// Parse a human byte size: "100MB", "50mb", "1.5g", "1048576" -> bytes (0 on fail).
export function parseByteSize(input) {
  if (typeof input === 'number') return Math.max(0, Math.floor(input));
  const s = String(input || '').trim();
  const m = s.match(/^([0-9]*\.?[0-9]+)\s*([kmgt]?b?)?$/i);
  if (!m) return 0;
  const num = parseFloat(m[1]);
  const unit = (m[2] || '').toLowerCase();
  const mult = unit.startsWith('k') ? 1024
    : unit.startsWith('m') ? 1024 ** 2
    : unit.startsWith('g') ? 1024 ** 3
    : unit.startsWith('t') ? 1024 ** 4
    : 1;
  return Math.floor(num * mult);
}

export function wasmHeapByteLength(native) {
  const heap = native && native.HEAPU8;
  if (!heap) return 0;
  return heap.buffer ? heap.buffer.byteLength : heap.length;
}

function wasmHeapLimitBytes(opts) {
  const limit = Number((opts && opts.wasmHeapLimitBytes) || 0);
  return Number.isFinite(limit) && limit > 0 ? Math.floor(limit) : 0;
}

function wasmBulkAssetBudgetBytes(native, opts) {
  const limit = wasmHeapLimitBytes(opts);
  if (!limit) return 0;
  const currentHeap = wasmHeapByteLength(native);
  if (currentHeap > limit) return 0;
  const reserve = Math.max(128 * 1024 * 1024, Math.floor(limit * 0.25));
  return Math.max(0, limit - reserve);
}

function createWasmBulkAssetGuard(native, opts, log) {
  const limit = wasmHeapLimitBytes(opts);
  if (!limit) return () => {};
  const budget = wasmBulkAssetBudgetBytes(native, opts);
  let registered = 0;
  return (name, bytes, phase = 'asset cache') => {
    const size = bytes && bytes.length ? bytes.length : 0;
    if (registered + size > budget) {
      const heapMiB = Math.round(wasmHeapByteLength(native) / 1048576);
      const limitMiB = Math.round(limit / 1048576);
      const budgetMiB = Math.round(budget / 1048576);
      throw new Error(
        `WASM heap cap would be exceeded while registering ${phase} "${name}" ` +
        `(${Math.round(size / 1048576)} MiB). Current heap ${heapMiB} MiB, ` +
        `configured cap ${limitMiB} MiB, bulk asset budget ${budgetMiB} MiB. ` +
        'Use the streaming pipeline or resize/re-encode textures.');
    }
    registered += size;
  };
}

// Replace a path's extension (keeping directories): "a/b.png","jpg" -> "a/b.jpg".
export function replaceExt(name, ext) {
  const dot = name.lastIndexOf('.');
  const slash = name.lastIndexOf('/');
  if (dot > slash) return name.slice(0, dot + 1) + ext;
  return name + '.' + ext;
}

// Instantiate the Emscripten module. `importer` returns the dynamic import of
// the compiled glue (e.g. () => import('./src/lightusd/lightusd.js')).
export async function loadWasm(importer, options = {}) {
  const mod = await importer();
  return await mod.default(options);
}

// Unpack a USDZ archive (Uint8Array) into { entries: Map<name, Uint8Array>,
// order: string[] }. USDZ mandates STORE (no compression) and 64-byte data
// alignment, so we only walk the central directory — no inflate needed.
// Throws on a compressed entry or a malformed archive.
export function unpackUSDZ(bytes) {
  const entries = new Map();
  const order = [];
  for (const entry of parseUSDZEntries(bytes)) {
    entries.set(entry.name, entry.data);
    order.push(entry.name);
  }
  return { entries, order };
}

// Expand any .usdz archives in an asset map into their contents, so internal
// textures can be repacked. Returns { assetMap, innerRoot } where innerRoot is
// the first USD layer found inside a .usdz (USDZ guarantees the first archive
// entry is the default root layer). Separately-supplied (non-.usdz) files are
// overlaid on top so they can override archive contents.
export function expandUsdzInputs(assetMap, opts = {}) {
  const log = opts.log || (() => {});
  const out = new Map();
  let innerRoot = null;
  // Pass 1: unpack archives.
  for (const [path, data] of assetMap) {
    if (!/\.usdz$/i.test(path)) continue;
    let unpacked;
    try {
      unpacked = unpackUSDZ(data);
    } catch (err) {
      log(`WARN: could not unpack ${path}: ${err && err.message ? err.message : err}`);
      out.set(path, data);
      continue;
    }
    for (const name of unpacked.order) out.set(name, unpacked.entries.get(name));
    const root = usdRootEntry(unpacked.order.map((name) => ({ name, data: unpacked.entries.get(name) }))).name;
    if (root && !innerRoot) innerRoot = root;
    log(`Unpacked ${path}: ${unpacked.order.length} entr${unpacked.order.length === 1 ? 'y' : 'ies'}` +
        (root ? ` (root layer: ${root})` : ''));
  }
  // Pass 2: overlay non-archive inputs (override archive contents).
  for (const [path, data] of assetMap) {
    if (/\.usdz$/i.test(path)) continue;
    out.set(path, data);
  }
  return { assetMap: out, innerRoot };
}

// Pick the root USD layer from an asset map (Map<path, bytes>).
// Prefers shallow paths and non-.usdz layers (a .usdz is self-contained).
export function rootUsdFromMap(assetMap, preferred) {
  if (preferred && assetMap.has(preferred)) return preferred;
  const usds = [...assetMap.keys()].filter(isUsdName);
  usds.sort((a, b) => {
    const za = /\.usdz$/i.test(a) ? 1 : 0;
    const zb = /\.usdz$/i.test(b) ? 1 : 0;
    if (za !== zb) return za - zb; // non-usdz first
    return a.split('/').length - b.split('/').length; // shallower first
  });
  return usds[0] || null;
}

function hasLegacyConverter(native) {
  return !!native && typeof native.LightUSDLoaderNative === 'function';
}

function hasCoreOnlyConverter(native) {
  return !!native && typeof native.USDZConverterNative === 'function';
}

function assertCoreOnlyUSDZConvertOptions(assetMap, rootPath, opts, textureFormat) {
  const material = String(opts.optimizeMaterials || opts.materialOptimization || 'off').toLowerCase();
  const geometry = String(opts.optimizeGeometry || opts.optimizeMeshes || opts.geometryOptimization || 'off').toLowerCase();
  if (!['off', 'none', 'dedupe', 'preview', 'atlas'].includes(material) || !['off', 'none', 'mergemeshes'].includes(geometry))
    throw new Error('The requested material/geometry optimization mode has not yet been migrated to core');

}

function coreNeedsFlatten(opts) {
  return opts.flatten !== false || !!opts.arkitCompatible || isMaterialOptimizationEnabled(opts) ||
    isGeometryOptimizationEnabled(opts) || Object.keys(variantSelectionsFromOptions(opts)).length > 0;
}

function coreMemoryLimit(opts) {
  const limit = opts.maxMemory || (opts.maxMemMb ? Number(opts.maxMemMb) * 1024 * 1024 : 0);
  if (limit && (!Number.isSafeInteger(limit) || limit < 1 || limit > 1024 * 1024 * 1024))
    throw new RangeError('Core conversion memory limit must be 1 byte through 1 GiB');
  return limit;
}

function coreSourceKey(value) {
  let path = String(value).replace(/\\/g, '/');
  const scheme = path.match(/^([a-z][a-z0-9+.-]*:\/\/[^/]*)(.*)$/i);
  const prefix = scheme ? scheme[1] : '';
  if (scheme) path = scheme[2];
  const absolute = path.startsWith('/');
  const parts = [];
  for (const part of path.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..' && parts.length && parts.at(-1) !== '..') parts.pop();
    else parts.push(part);
  }
  return prefix + (absolute ? '/' : '') + parts.join('/');
}

function coreSourceIndex(source) {
  const index = new Map();
  for (const key of source.keys) {
    const normalized = coreSourceKey(key);
    if (index.has(normalized) && index.get(normalized) !== key)
      throw new Error(`Ambiguous source paths: ${index.get(normalized)} and ${key}`);
    index.set(normalized, key);
  }
  return index;
}

async function coreFlattenSource(native, source, rootPath, opts) {
  const session = new native.FlattenSession();
  const index = coreSourceIndex(source);
  try {
    const limit = coreMemoryLimit(opts);
    if (limit) {
      const result = session.setMaxInputBytes(limit);
      if (!result.success) throw new Error(result.error);
    }
    const outputLimit = session.setMaxOutputBytes(maxUSDCExportBytes(opts));
    if (!outputLimit.success) throw new Error(outputLimit.error);
    for (const [key, selection] of Object.entries(variantSelectionsFromOptions(opts)))
      session.setVariantOverride(key, selection);
    const begun = session.begin(await source.fetch(rootPath, {maxBytes:session.maxInputBytes()}), coreSourceKey(rootPath), opts.eager !== true);
    if (!begun.success) throw new Error(begun.error || session.error());
    // Keep source-relative directories in the archive. Anchored remapping must
    // happen before serialization discards the source layer of each prim. The
    // identity root entry also enables rebasing when there are no local images.
    session.setAssetPathRemap(Object.fromEntries(source.keys.map(key => [coreSourceKey(key), coreSourceKey(key)])));
    const provided = new Set();
    for (let iteration = 0; iteration < source.keys.length * 2 + 16; ++iteration) {
      const step = session.step(null);
      if (!step.success) throw new Error(step.error || 'Core composition failed');
      if (step.status === 'done') {
        if (step.compositionErrors?.length) throw new Error(step.compositionErrors.join('\n'));
        if (!step.data?.length) throw new Error('Core composition produced an empty layer');
        return step;
      }
      if (step.status === 'ready') continue;
      if (step.status !== 'need-layer') throw new Error(`Unexpected core composition status: ${step.status}`);
      const key = coreSourceKey(step.key);
      const original = index.get(key);
      if (original === undefined) throw new Error(`Missing USD dependency layer: ${step.key}`);
      if (provided.has(key)) throw new Error(`Core composition requested a provided layer again: ${step.key}`);
      const result = session.provideLayer(step.key, await source.fetch(original,
        {maxBytes: Math.max(0, session.maxInputBytes() - session.inputBytes())}));
      if (!result.success) throw new Error(result.error || session.error());
      provided.add(key);
    }
    throw new Error('Core composition exceeded its dependency iteration limit');
  } finally {
    session.end();
    session.delete();
  }
}

function coreExportRoot(converter, opts, rootLayerFormat, remap, stagedImages) {
  if (opts.arkitCompatible || (remap && Object.keys(remap).length) || isMaterialOptimizationEnabled(opts) || isGeometryOptimizationEnabled(opts)) {
    const packageBytes = exportUSDZ(converter, remap, {...opts, rootLayerFormat});
    if (!packageBytes?.length) throw new Error(converter.error() || 'Core optimized export failed');
    const archive = unpackUSDZ(packageBytes);
    const atlases = archive.order.slice(1).filter(name => !stagedImages.has(name))
      .map(name => ({name, data:archive.entries.get(name)}));
    return {data:archive.entries.get(archive.order[0]), atlases};
  }
  const data = rootLayerFormat === 'usda'
    ? new TextEncoder().encode(converter.exportAsUSDA()) : converter.exportAsUSDC();
  return {data, atlases:[]};
}

function configureCoreConverter(converter, opts) {
  const limit = coreMemoryLimit(opts);
  if (limit) converter.setMaxRetainedPayloadBytes(limit);
  if (opts.maxUsdcMb > 0 || opts.maxMemMb > 0)
    converter.setUSDCExportLimitMB(opts.maxUsdcMb || 0, opts.maxMemMb || 0);
}

function coreImageOptions(opts, format, colorspace, overrides = {}) {
  return {format, maxSize:opts.maxTextureSize || 0, pngEncoder:opts.pngEncoder || 'auto',
    jpegQuality:opts.jpegQuality || 90, resizeColorspace:colorspace,
    lowMemory:opts.lowMemoryTextures ? 1 : 0,
    ...(opts.imageMemoryBudgetBytes ? {memoryBudgetBytes:opts.imageMemoryBudgetBytes} : {}), ...overrides};
}
function coreAssetPlans(entries, opts) {
  return entries.map(entry => {
    const image=isImageName(entry.name);
    let format=image?outputFormatForImage(entry.name,opts.textureFormat):{format:null,ext:null};
    if(image && opts.arkitCompatible && !['png','jpeg'].includes(format.format))
      format={format:'png',ext:'png'};
    const mandatory=/\.ktx2$/i.test(entry.name) ||
      (image && !!opts.arkitCompatible && !/\.(png|jpe?g)$/i.test(entry.name));
    const work=image && (!entry.generated || mandatory) && !!format.format &&
      (mandatory || opts.maxTextureSize>0 || opts.reencode===true || normalizedTextureFormat(opts.textureFormat)!=='keep');
    return {...entry, image, format:format.format,
      outName:work?replaceExt(entry.name,format.ext):entry.name, work, mandatory};
  });
}
async function coreAssetOne(native, plan, opts, pickColorspace, log, streaming = false) {
  const bytes=plan.data || await plan.fetch();
  if(!ArrayBuffer.isView(bytes))throw new TypeError(`Asset source did not return byte data: ${plan.name}`);
  let result={name:plan.outName,data:bytes,resized:false,reencoded:false};
  if(!plan.image) {
    if(isAudioName(plan.name) && typeof opts.audioProcessor==='function') {
      try {const processed=await opts.audioProcessor({path:plan.path,name:plan.name,data:bytes,log});
        if(processed?.data)result.data=new Uint8Array(processed.data);
      } catch(error){log(`Audio processor failed for ${plan.name}: ${error.message}; copying original bytes`);}
    }
    return result;
  }
  if(plan.generated && !plan.work)return result;
  if(typeof opts.textureProcessor==='function') {
    try {
      const processed=await opts.textureProcessor({path:plan.path,name:plan.name,data:bytes,
        maxTextureSize:opts.maxTextureSize || 0,reencode:opts.reencode,
        textureFormat:opts.arkitCompatible?plan.format:normalizedTextureFormat(opts.textureFormat),jpegQuality:opts.jpegQuality || 90,
        resizeColorspace:pickColorspace(plan.name),log});
      if(processed?.data) {
        const name=processed.name || (processed.ext?replaceExt(plan.name,processed.ext):plan.outName);
        if(opts.arkitCompatible && !/\.(png|jpe?g)$/i.test(name))
          throw new Error('ARKit texture processors must return PNG or JPEG');
        // Streaming archive names are chosen before its root entry is emitted,
        // as in the existing streaming converter. A codec must honor that plan.
        if(streaming && processed.ext && imageFormatFromName(name)!==imageFormatFromName(plan.outName))
          throw new Error('Texture processor changed the planned streaming format');
        return {name:streaming?plan.outName:name,data:new Uint8Array(processed.data),
          resized:!!processed.resized,reencoded:true};
      }
    } catch(error){log(`Texture processor failed for ${plan.name}: ${error.message}; trying WASM`);}
  }
  if(!plan.work)return result;
  const converted=native.convertImage(bytes,coreImageOptions(opts,plan.format,pickColorspace(plan.name)));
  if(!converted?.success) {
    // A format-changing failure cannot be packaged under the planned new name.
    if(plan.mandatory || plan.outName!==plan.name)throw new Error(`Texture conversion failed for ${plan.name}: ${converted?.error || 'unknown error'}`);
    log(`Texture conversion failed for ${plan.name}: ${converted?.error}; copying original bytes`);
    return {...result,name:plan.name};
  }
  return {...result,data:converted.data,resized:!!converted.resized,reencoded:true};
}
async function coreFitAssetPlans(native, plans, opts, pickColorspace, log) {
  const images=plans.filter(plan=>plan.image);
  const target=Number(opts.targetTextureBytes || 0);
  if(!Number.isSafeInteger(target) || target<0)throw new RangeError('targetTextureBytes must be a nonnegative integer');
  if(!target || !images.length)return null;
  const quality=opts.fitStrategy==='quality';
  const minQuality=opts.fitMinQuality ?? 30,minSize=opts.fitMinTextureSize ?? 64;
  if(!Number.isSafeInteger(minQuality) || minQuality<1 || minQuality>100 ||
     !Number.isSafeInteger(minSize) || minSize<1 || minSize>16384)throw new RangeError('Invalid texture fit floor');
  let fixed=0,maxDimension=0;
  // Probe/replay instead of retaining every decoded input at once. This keeps
  // the streaming budget fitter bounded to one image across all search passes.
  const encode=async(plan,knob,initial=false)=>{
    const bytes=plan.data || await plan.fetch();
    if(plan.fixed)return {data:bytes,name:plan.name,resized:false,reencoded:false};
    const format=quality?'jpeg':(plan.format || outputFormatForImage(plan.name,'keep').format);
    if(!format){plan.fixed=true;plan.outName=plan.name;fixed+=bytes.byteLength;return {data:bytes,name:plan.name,resized:false,reencoded:false};}
    const converted=native.convertImage(bytes,coreImageOptions(opts,format,pickColorspace(plan.name),{
      maxSize:quality?(opts.maxTextureSize || 0):(initial?(opts.maxTextureSize || 0):knob),
      jpegQuality:quality?knob:(opts.jpegQuality || 90),
    }));
    if(!converted.success) {
      if(plan.mandatory)throw new Error(`Texture conversion failed for ${plan.name}: ${converted.error}`);
      if(!initial)throw new Error(`Texture fit failed for ${plan.name}: ${converted.error}`);
      plan.fixed=true;plan.outName=plan.name;fixed+=bytes.byteLength;log(`Fixed texture ${plan.name}: ${converted.error}`);
      return {data:bytes,name:plan.name,resized:false,reencoded:false};
    }
    if(initial)maxDimension=Math.max(maxDimension,converted.width,converted.height);
    const ext=format==='jpeg'?'jpg':format;
    if(initial)plan.outName=replaceExt(plan.name,ext);
    return {name:plan.outName,data:converted.data,resized:converted.resized,reencoded:true};
  };
  let total=0;
  const highQuality=Math.max(95,minQuality);
  for(const plan of images)total+=(await encode(plan,highQuality,true)).data.byteLength;
  const low=quality?minQuality:Math.min(minSize,opts.maxTextureSize || minSize);
  const high=quality?highQuality:Math.max(low,maxDimension);
  let best=high;
  const probe=async knob=>{let sum=0;for(const plan of images)sum+=(await encode(plan,knob)).data.byteLength;return sum;};
  if(fixed>=target)best=low;
  else if(total>target) {
    let a=low,b=high;best=low;
    for(let iteration=0;iteration<16 && a<=b;iteration++) {
      const mid=a+Math.floor((b-a)/2);
      if(await probe(mid)<=target){best=mid;a=mid+1;}else b=mid-1;
    }
  }
  const finalTotal=await probe(best);
  if(finalTotal>target)log(`Could not fit textures within ${target} bytes; using the smallest allowed setting (${finalTotal} bytes)`);
  return {totalBytes:finalTotal,process:plan=>encode(plan,best)};
}
function coreUniqueAssetNames(plans) {
  const claimed=new Set(['root.usda','root.usdc']);
  const names=new Array(plans.length);
  for(let i=0;i<plans.length;i++)if(plans[i].name===plans[i].outName && !claimed.has(plans[i].outName)){
    names[i]=plans[i].outName;claimed.add(names[i]);
  }
  for(let i=0;i<plans.length;i++)if(names[i]===undefined){
    const proposed=plans[i].outName;let name=proposed,suffix=0;
    while(claimed.has(name)){
      const dot=proposed.lastIndexOf('.'),slash=proposed.lastIndexOf('/');
      name=dot>slash?proposed.slice(0,dot)+'_'+(++suffix)+proposed.slice(dot):proposed+'_'+(++suffix);
    }
    names[i]=name;claimed.add(name);
  }
  return names;
}
async function coreProcessAssets(native, entries, rootBytes, opts, log, progress) {
  const plans=coreAssetPlans(entries,opts),pick=makeResizeCsPicker(native,rootBytes,opts);
  const fit=await coreFitAssetPlans(native,plans,opts,pick,log);
  const results=new Array(plans.length),stats={resized:0,reencoded:0};let core=0,completed=0;
  const worker=async()=>{while(core<plans.length){const index=core++,plan=plans[index];
    const result=fit && plan.image?await fit.process(plan):await coreAssetOne(native,plan,opts,pick,log);
    results[index]=result;if(result.resized)stats.resized++;if(result.reencoded)stats.reencoded++;
    progress('textures',++completed,plans.length,'Processing package assets',result.name);
  }};
  await Promise.all(Array.from({length:Math.min(plans.length,normalizeTextureConcurrency(opts.textureConcurrency,1))},worker));
  const remap={};
  const chosen=coreUniqueAssetNames(results.map((result,i)=>({name:plans[i].name,outName:result.name})));
  for(let i=0;i<results.length;i++){
    results[i].name=chosen[i];if(plans[i].name!==chosen[i])remap[plans[i].name]=chosen[i];
  }
  if(fit)stats.fitTotalBytes=fit.totalBytes;
  return {entries:results,remap,stats};
}

// Retained layers keep their original archive directories and bytes, including
// layers outside the root's directory. Changing the root format keeps an alias
// of the original root so back-references continue to resolve unchanged.
async function corePackageRetainedLayers(native, source, opts, rootPath, streaming = false) {
  if (udimOptions(opts).mode !== 'off')
    throw new Error('UDIM baking across retained dependency layers requires flattening');
  const index = coreSourceIndex(source);
  for (const key of index.keys())
    if (!key || key.startsWith('/') || key.split('/').includes('..') || /[:\0]/.test(key))
      throw new Error(`Unsafe retained-layer archive path: ${key}`);
  const rootKey = coreSourceKey(rootPath), originalRoot = index.get(rootKey);
  if (originalRoot === undefined) throw new Error(`Missing root layer: ${rootPath}`);
  const format = opts.rootLayerFormat === 'usda' ? 'usda' : 'usdc';
  const limit = coreMemoryLimit(opts) || 512 * 1024 * 1024;
  const rootInput = await source.fetch(originalRoot,{maxBytes:limit});
  if (!ArrayBuffer.isView(rootInput) || rootInput.byteLength > limit)
    throw new Error('Retained root exceeds configured input byte limit');
  const input = new Uint8Array(rootInput.buffer,rootInput.byteOffset,rootInput.byteLength);
  const converter = new native.USDZConverterNative();
  let root;
  try {
    configureCoreConverter(converter,opts);
    if (!converter.loadFromBinary(input,rootKey)) throw new Error(converter.error());
    const bytes = format === 'usda' ? new TextEncoder().encode(converter.exportAsUSDA()) : converter.exportAsUSDC();
    if (!bytes?.length) throw new Error(converter.error() || 'Retained root export failed');
    root = new Uint8Array(bytes);
  } finally {converter.delete();}
  if(root.length>limit)throw new Error('Retained root exceeds configured output byte limit');
  const base = rootKey.replace(/\.(usd|usda|usdc)$/i,'');
  let rootName = base + '.' + format;
  for (let suffix=1; rootName !== rootKey && index.has(rootName); ++suffix)
    rootName = base + '-converted-' + suffix + '.' + format;
  const chunks = [], writer = new ZipStreamWriter(opts.zipSink || (bytes=>chunks.push(bytes.slice())));
  const log = opts.log || (()=>{});
  log('Retaining dependency layers and asset bytes; ordinary texture/audio processing is disabled without flattening.');
  writer.addEntry(rootName,root);
  let textures=0,audio=0,otherAssets=0,layersPackaged=0;
  for (const [name,key] of index) {
    if (name === rootName) continue;
    const bytes = name === rootKey ? input : await source.fetch(key,{maxBytes:Math.max(0,limit-root.length)});
    if (!ArrayBuffer.isView(bytes) || bytes.byteLength > limit-root.length)
      throw new Error(`Retained asset exceeds configured input byte limit: ${name}`);
    writer.addEntry(name,new Uint8Array(bytes.buffer,bytes.byteOffset,bytes.byteLength));
    if (isUsdName(name)) layersPackaged++;
    else if (isImageName(name)) textures++;
    else if (isAudioName(name)) audio++;
    else otherAssets++;
  }
  writer.finalize();
  let usdz=null;
  if (!opts.zipSink) {
    usdz=new Uint8Array(chunks.reduce((total,chunk)=>total+chunk.length,0));
    let offset=0;for(const chunk of chunks){usdz.set(chunk,offset);offset+=chunk.length;}
  }
  opts.progress?.({stage:'complete',current:1,total:1,message:'Retained-layer USDZ package ready',path:rootName});
  return {usdz,streamedToSink:!!opts.zipSink,stats:{pipeline:'core',flatten:false,
    streaming,singleLayerRewrite:false,rootPath,rootLayerFormat:format,
    rootBytes:root.length,layersPackaged,textures,audio,otherAssets,resized:0,reencoded:0}};
}

async function convertFolderToUSDZCoreOnly(native, inputAssetMap, opts, log, reportProgress) {
  let assetMap = inputAssetMap;
  let rootPath = opts.rootPath || rootUsdFromMap(assetMap);
  if (!rootPath) throw new Error('No USD file (.usd/.usda/.usdc/.usdz) found in the input.');

  if (/\.usdz$/i.test(rootPath)) {
    const expanded = expandUsdzInputs(assetMap, { log });
    if (expanded.innerRoot || [...expanded.assetMap.keys()].some(isUsdName)) {
      assetMap = expanded.assetMap;
      rootPath = expanded.innerRoot || rootUsdFromMap(assetMap);
      if (!rootPath) throw new Error('USDZ archive contained no USD layer.');
      log(`Repacking USDZ with core module; inner root layer: ${rootPath}`);
    }
  }

  if (!coreNeedsFlatten(opts) && [...assetMap.keys()].some(key=>key!==rootPath && isUsdName(key)))
    return corePackageRetainedLayers(native,{keys:[...assetMap.keys()],fetch:async key=>assetMap.get(key)},opts,rootPath);
  await loadTextureFeatures(native,assetMap.keys(),opts);
  const textureFormat = normalizedTextureFormat(opts.textureFormat);
  assertCoreOnlyUSDZConvertOptions(assetMap, rootPath, opts, textureFormat);

  const rootLayerFormat =
    !opts.arkitCompatible && String(opts.rootLayerFormat || 'usdc').toLowerCase() === 'usda' ? 'usda' : 'usdc';
  const flatten = coreNeedsFlatten(opts);
  const rootDir = !flatten && rootPath.includes('/') ? rootPath.slice(0, rootPath.lastIndexOf('/') + 1) : '';
  const assetNameFor = (path) => flatten ? coreSourceKey(path) :
    ((rootDir && path.startsWith(rootDir)) ? path.slice(rootDir.length) : path);

  const passthroughEntries = [];
  let textures = 0;
  let audio = 0;
  let otherAssets = 0;
  for (const path of assetMap.keys()) {
    if (path === rootPath || isUsdName(path)) continue;
    const name = assetNameFor(path);
    const data = assetMap.get(path);
    passthroughEntries.push({ name, data, path });
    if (isImageName(path)) textures++;
    else if (isAudioName(path)) audio++;
    else otherAssets++;
  }

  const converter = new native.USDZConverterNative();
  try {
    configureCoreConverter(converter, opts);
    const source = {keys: [...assetMap.keys()], fetch: async key => assetMap.get(key)};
    reportProgress('flatten', 0, 2, flatten ? 'Composing USD layers' : 'Parsing root USD layer', rootPath);
    const flattened = flatten ? await coreFlattenSource(native, source, rootPath, opts) : null;
    const referenced = flattened ? new Set(flattened.assetPaths.map(coreSourceKey)) : null;
    const rootBytes = flattened ? flattened.data : assetMap.get(rootPath);
    if (!converter.loadFromBinary(rootBytes, flatten ? 'root.usdc' : rootPath.split('/').pop()))
      throw new Error(converter.error());
    let baked = null;
    if (udimOptions(opts).mode !== 'off')
      baked = await bakeLayerUDIM(native, converter, source, opts, rootDir);
    const baseRoot=rootLayerFormat==='usda'?new TextEncoder().encode(converter.exportAsUSDA()):converter.exportAsUSDC();
    const retained=passthroughEntries.filter(entry => !referenced || !isImageName(entry.name) ||
      opts.includeUnusedTextures !== false || referenced.has(coreSourceKey(entry.name)));
    const selected=baked?retained.filter(entry=>opts.includeUnusedTextures===true || !baked.consumed.has(entry.path))
      .concat(baked.assets.map(entry=>({...entry,generated:true}))):retained;
    const processed=await coreProcessAssets(native,selected,baseRoot,opts,log,reportProgress);
    const stagedImages = new Set();
    if (isMaterialAtlasEnabled(opts)) {
      for (const entry of selected) {
        if (!isImageName(entry.name)) continue;
        if (!converter.setAsset(entry.name, entry.data))
          throw new Error(converter.error() || `Could not stage image ${entry.name}`);
        stagedImages.add(processed.remap[entry.name] || entry.name);
      }
    }
    const {data, atlases} = coreExportRoot(converter, opts, rootLayerFormat, processed.remap, stagedImages);
    if(!data?.length)throw new Error(converter.error() || 'Core root export failed');
    if(converter.warn?.())log('WARN: '+converter.warn());
    const rewrittenRoot=new Uint8Array(data),rootName='root.'+rootLayerFormat,entries=processed.entries.concat(atlases);
    reportProgress('package',0,1,'Writing USDZ package',rootName);
    let usdz = null;
    if (opts.zipSink) {
      const writer = new ZipStreamWriter(opts.zipSink);
      writer.addEntry(rootName, rewrittenRoot);
      for (const entry of entries) writer.addEntry(entry.name, entry.data);
      writer.finalize();
    } else usdz = buildUSDZWithNewRoot(rootName, rewrittenRoot, entries);
    reportProgress('complete', 1, 1, 'USDZ package ready', rootName);
    return {
      usdz,
      streamedToSink: !!opts.zipSink,
      stats: {
        textures: entries.filter(entry => isImageName(entry.name)).length,
        ...processed.stats,
        audio,
        otherAssets,
        rootPath,
        rootLayerFormat,
        flatten,
        arkitCompatible: !!opts.arkitCompatible,
        pipeline: 'core',
        assetPathsRemapped: Object.keys(processed.remap).length,
        unusedTexturesIncluded: referenced ? selected.filter(entry => isImageName(entry.name) && !entry.generated && !referenced.has(coreSourceKey(entry.name))).length : 0,
        udimSets: baked?.jobs.length || 0, udimTiles: baked?.tiles || 0,
        singleLayerRewrite: !flatten,
        rootBytes: rewrittenRoot.length,
      },
    };
  } finally {
    converter.delete();
  }
}

async function convertSourceToUSDZCoreOnly(native, source, opts, rootPath) {
  const keys = source.keys;
  if (!coreNeedsFlatten(opts) && keys.some(key=>key!==rootPath && isUsdName(key)))
    return corePackageRetainedLayers(native,source,opts,rootPath,true);
  await loadTextureFeatures(native,keys,opts);
  assertCoreOnlyUSDZConvertOptions(new Map(keys.map(key => [key, null])), rootPath,
    opts, normalizedTextureFormat(opts.textureFormat));
  const flatten = coreNeedsFlatten(opts);
  const rootDir = !flatten && rootPath.includes('/') ? rootPath.slice(0, rootPath.lastIndexOf('/') + 1) : '';
  const rootLayerFormat = !opts.arkitCompatible && opts.rootLayerFormat === 'usda' ? 'usda' : 'usdc';
  const converter = new native.USDZConverterNative();
  try {
    configureCoreConverter(converter, opts);
    const flattened = flatten ? await coreFlattenSource(native, source, rootPath, opts) : null;
    const referenced = flattened ? new Set(flattened.assetPaths.map(coreSourceKey)) : null;
    const bytes = flattened ? flattened.data : await source.fetch(rootPath);
    if (!converter.loadFromBinary(bytes, flatten ? 'root.usdc' : rootPath.split('/').pop())) throw new Error(converter.error());
    const baked = await bakeLayerUDIM(native, converter, source, opts, rootDir, false);
    const streamingOpts={...opts,reencode:opts.reencode!==false};
    const generated=baked.jobs.map(job=>({name:job.name,generated:true,path:job.name,fetch:async()=>{
      const atlas=await bakeUDIMAtlas(native,source,job.site.pattern,{...opts,udimSharedLayout:job.sharedLayout},rootDir,job.site.srgb);
      if(atlas.name!==job.digestName)throw new Error('UDIM source changed during streaming conversion');
      return atlas.data;
    }}));
    const original=keys.filter(key=>!isUsdName(key) && (!baked.consumed.has(key) || opts.includeUnusedTextures===true) &&
      (!referenced || !isImageName(key) || opts.includeUnusedTextures !== false || referenced.has(coreSourceKey(key))))
      .map(key=>({path:key,name:flatten?coreSourceKey(key):(rootDir && key.startsWith(rootDir)?key.slice(rootDir.length):key),
        fetch:maxBytes=>source.fetch(key,maxBytes === undefined ? undefined : {maxBytes})}));
    const plans=coreAssetPlans(generated.concat(original),streamingOpts);
    const baseRoot=rootLayerFormat==='usda'?new TextEncoder().encode(converter.exportAsUSDA()):converter.exportAsUSDC();
    const pick=makeResizeCsPicker(native,baseRoot,opts),log=opts.log || (()=>{});
    const fit=await coreFitAssetPlans(native,plans,streamingOpts,pick,log);
    const chosen=coreUniqueAssetNames(plans);const remap={};
    for(let i=0;i<plans.length;i++){plans[i].outName=chosen[i];if(plans[i].name!==chosen[i])remap[plans[i].name]=chosen[i];}
    const stagedImages = new Set();
    if (isMaterialAtlasEnabled(opts)) {
      for (const plan of plans) {
        if (!plan.image) continue;
        const budget = converter.maxAssetBytes() - converter.assetBytes();
        const data = await plan.fetch(budget);
        if (!converter.setAsset(plan.name, data))
          throw new Error(converter.error() || `Could not stage image ${plan.name}`);
        stagedImages.add(plan.outName);
      }
    }
    const {data:root, atlases} = coreExportRoot(converter, opts, rootLayerFormat, remap, stagedImages);
    if(!root?.length)throw new Error(converter.error() || 'Root export failed');
    if(converter.warn?.())log('WARN: '+converter.warn());
    const chunks=[],writer=new ZipStreamWriter(opts.zipSink || (bytes=>chunks.push(bytes.slice())));
    writer.addEntry('root.'+rootLayerFormat,root);
    let textures=0,resized=0,reencoded=0,audio=0,otherAssets=0;
    for(const plan of plans) {
      const result=fit && plan.image?await fit.process(plan):await coreAssetOne(native,plan,streamingOpts,pick,log,true);
      writer.addEntry(result.name,result.data);
      if(plan.image)textures++;else if(isAudioName(plan.name))audio++;else otherAssets++;
      if(result.resized)resized++;if(result.reencoded)reencoded++;
    }
    for (const atlas of atlases) {
      writer.addEntry(atlas.name,atlas.data);
      textures++;
    }
    writer.finalize();
    let usdz = null;
    if (!opts.zipSink) {
      usdz = new Uint8Array(chunks.reduce((total, chunk) => total + chunk.length, 0));
      let offset = 0; for (const chunk of chunks) { usdz.set(chunk, offset); offset += chunk.length; }
    }
    return {usdz, streamedToSink: !!opts.zipSink, stats: {pipeline: 'core', streaming: true,
      flatten, assetPathsRemapped: Object.keys(remap).length,
      unusedTexturesIncluded: referenced ? original.filter(entry => isImageName(entry.name) && !referenced.has(coreSourceKey(entry.name))).length : 0, arkitCompatible:!!opts.arkitCompatible, singleLayerRewrite: !flatten, rootLayerFormat, textures,resized,reencoded,audio,otherAssets,
      ...(fit?{fitTotalBytes:fit.totalBytes}:{}),udimSets: baked.jobs.length, udimTiles: baked.tiles}};
  } finally { converter.delete(); }
}

// Convert an asset map (Map<path, Uint8Array>) into a USDZ Uint8Array.
//
function isMaterialAtlasEnabled(opts) {
  return String(opts.optimizeMaterials || opts.materialOptimization || '').toLowerCase() === 'atlas';
}

function isMaterialOptimizationEnabled(opts) {
  const mode = String((opts && (opts.optimizeMaterials || opts.materialOptimization)) || 'off').toLowerCase();
  return mode !== 'off' && mode !== 'none';
}

function isGeometryOptimizationEnabled(opts) {
  const mode = String((opts && (opts.optimizeGeometry || opts.optimizeMeshes || opts.geometryOptimization)) || 'off').toLowerCase();
  return mode !== 'off' && mode !== 'none';
}

function exportUSDZ(usd, remap, opts) {
  const rootLayerFormat = String(opts.rootLayerFormat || 'usdc').toLowerCase() === 'usda' ? 'usda' : 'usdc';
  const materialMode = String(opts.optimizeMaterials || opts.materialOptimization || 'off').toLowerCase();
  const optimizeMaterials = materialMode === 'none' ? 'off' : materialMode;
  const geometryMode = String(opts.optimizeGeometry || opts.optimizeMeshes || opts.geometryOptimization || 'off').toLowerCase();
  const optimizeGeometry = geometryMode === 'mergemeshes' ? 'mergeMeshes' : geometryMode === 'none' ? 'off' : geometryMode;
  const materialOptimizationEnabled = isMaterialOptimizationEnabled(opts);
  const geometryOptimizationEnabled = isGeometryOptimizationEnabled(opts);
  const exportOpts = {
    rootLayerFormat: opts.arkitCompatible ? 'usdc' : rootLayerFormat,
    arkitCompatible: !!opts.arkitCompatible,
    optimizeMaterials,
    optimizeGeometry,
    materialAtlasSize: opts.materialAtlasSize ?? 4096,
    materialAtlasTileSize: opts.materialAtlasTileSize ?? 512,
    materialAtlasPadding: opts.materialAtlasPadding == null ? 2 : opts.materialAtlasPadding,
    materialAtlasMinGroupSize: opts.materialAtlasMinGroupSize ?? 2,
    meshMergeMaxInputFaces: opts.meshMergeMaxInputFaces || 2048,
    meshMergeMaxInputPoints: opts.meshMergeMaxInputPoints || 4096,
    meshMergeMaxAggregateFaces: opts.meshMergeMaxAggregateFaces || 65536,
    meshMergeMinGroupSize: opts.meshMergeMinGroupSize || 2,
  };
  if (opts.flatten === false && !opts.arkitCompatible &&
      !materialOptimizationEnabled && !geometryOptimizationEnabled &&
      typeof usd.exportLayerAsUSDZWithOptions === 'function') {
    return usd.exportLayerAsUSDZWithOptions({ ...exportOpts, rootLayerFormat: 'usda' });
  }
  const hasRemap = remap && Object.keys(remap).length > 0;
  // Faithful path: when no texture path remapping is needed (passthrough /
  // USDZ->USDZ roundtrip), write the composed LAYER directly. This preserves all
  // PrimSpec data and matches the native lusdzconvert output. The Stage-based
  // export below reconstructs typed Prims and drops typed shader inputs that lack
  // a USDC serializer (e.g. OpenPBR surface inputs), so only use it when a remap
  // or ARKit metadata rewrite is required.
  if (!hasRemap && !opts.arkitCompatible &&
      typeof usd.exportLayerAsUSDZWithOptions === 'function') {
    return usd.exportLayerAsUSDZWithOptions(exportOpts);
  }
  const hasOptions = exportOpts.rootLayerFormat !== 'usdc' ||
    exportOpts.arkitCompatible || materialOptimizationEnabled ||
    geometryOptimizationEnabled;
  if (typeof usd.exportAsUSDZWithOptions === 'function' && (hasRemap || hasOptions)) {
    return usd.exportAsUSDZWithOptions(remap || {}, exportOpts);
  }
  if (hasRemap) return usd.exportAsUSDZWithRemap(remap);
  return usd.exportAsUSDZ();
}

export function composeToFixedPoint(usd, progress = null) {
  const steps = [
    { has: 'hasSublayers', compose: 'composeSublayers', name: 'sublayers' },
    { has: 'hasReferences', compose: 'composeReferences', name: 'references' },
    { has: 'hasPayload', compose: 'composePayload', name: 'payloads' },
    { has: 'hasInherits', compose: 'composeInherits', name: 'inherits' },
    { has: 'hasVariants', compose: 'composeVariants', name: 'variants' },
  ];
  const total = 64 * steps.length;
  for (let iter = 0; iter < 64; iter++) {
    let didCompose = false;
    for (let stepIndex = 0; stepIndex < steps.length; stepIndex++) {
      const step = steps[stepIndex];
      if (typeof usd[step.has] !== 'function' ||
          typeof usd[step.compose] !== 'function') {
        continue;
      }
      if (!usd[step.has]()) {
        continue;
      }
      if (progress) {
        progress({
          iteration: iter + 1,
          step: step.name,
          current: iter * steps.length + stepIndex,
          total,
          message: `Composing ${step.name}`,
        });
      }
      if (!usd[step.compose]()) {
        throw new Error(`Failed to compose ${step.name}: ${usd.error()}`);
      }
      didCompose = true;
    }
    if (!didCompose) {
      return;
    }
  }
  throw new Error('Composition did not converge before the iteration limit.');
}

function loadLayerFromBinary(usd, bytes, filename) {
  if (typeof usd.loadAsLayerFromBinaryWithProgress === 'function') {
    return usd.loadAsLayerFromBinaryWithProgress(bytes, filename);
  }
  return usd.loadAsLayerFromBinary(bytes, filename);
}

function shouldUseLowHeapFlattenedUSDZ(rootPath, assetMap, opts, textureFormat) {
  const rootLayerFormat =
    String(opts.rootLayerFormat || 'usdc').toLowerCase() === 'usda' ? 'usda' : 'usdc';
  return /\.usdz$/i.test(rootPath) &&
    assetMap.size === 1 &&
    assetMap.has(rootPath) &&
    opts.lowHeapFlattenUsdz !== false &&
    opts.repackUsdz !== false &&
    opts.flatten !== false &&
    opts.reencode === false &&
    textureFormat === 'keep' &&
    rootLayerFormat === 'usdc' &&
    !isMaterialOptimizationEnabled(opts) &&
    !isGeometryOptimizationEnabled(opts) &&
    (opts.maxTextureSize || 0) <= 0 &&
    (opts.targetTextureBytes || 0) <= 0 &&
    !opts.arkitCompatible &&
    typeof opts.textureProcessor !== 'function' &&
    typeof opts.audioProcessor !== 'function';
}

// Like shouldUseLowHeapFlattenedUSDZ, but for the STAGE (typed-Prim,
// ARKit-style flatten) path: a single self-contained .usdz, ARKit-compatible,
// textures kept (no re-encode). Streams the typed-flattened root USDC into a JS
// buffer and repacks the zip in JS, so the in-heap stage path's 2 GB wasm32 OOM
// on large scenes (texture decode + whole crate + zip in heap) is avoided.
function shouldUseLowHeapStageFlattenedUSDZ(rootPath, assetMap, opts, textureFormat) {
  return /\.usdz$/i.test(rootPath) &&
    assetMap.size === 1 &&
    assetMap.has(rootPath) &&
    !!opts.arkitCompatible &&
    opts.lowHeapStageUsdz !== false &&
    opts.lowHeapFlattenUsdz !== false &&
    opts.repackUsdz !== false &&
    opts.flatten !== false &&
    opts.reencode === false &&
    textureFormat === 'keep' &&
    !isMaterialOptimizationEnabled(opts) &&
    !isGeometryOptimizationEnabled(opts) &&
    (opts.maxTextureSize || 0) <= 0 &&
    (opts.targetTextureBytes || 0) <= 0 &&
    typeof opts.textureProcessor !== 'function' &&
    typeof opts.audioProcessor !== 'function';
}

function maxUSDCExportBytes(opts) {
  const configuredMb = Number(opts.maxUsdcMb || 0);
  if (configuredMb > 0) return Math.floor(configuredMb * 1024 * 1024);
  return 1024 * 1024 * 1024;
}

function initialLowHeapRootCapacity(inputBytes, opts) {
  // The keep-textures root crate is geometry-only (textures pass through), so it
  // is usually <= the input .usdz. Start at 1.5x and let the too-small retry
  // (exportUSDCOutsideWasmHeap doubles) handle the rare inflating scene — much
  // tighter than the old 3x, which over-allocated ~600 MB for big scenes.
  const multiplier = Number(opts.lowHeapRootSizeMultiplier || 1.5);
  const minBytes = 64 * 1024 * 1024;
  const wanted = Math.max(minBytes, Math.ceil(inputBytes * Math.max(1, multiplier)));
  return Math.min(maxUSDCExportBytes(opts), wanted);
}

function isBufferTooSmallError(error) {
  return /buffer|capacity|too small|exceeds/i.test(String(error || ''));
}

// Export the composed root USDC straight into a JS buffer (outside the wasm
// heap). kind='layer' writes the composed Layer's PrimSpecs as-is (faithful);
// kind='stage' writes the typed-Prim-reconstructed Stage (the arkit/flatten
// path) — same streaming, so large typed-flatten scenes stay off the wasm heap.
function exportUSDCOutsideWasmHeap(usd, inputBytes, opts, log, kind = 'layer') {
  // 'layer': write the composed Layer's PrimSpecs (faithful; used for both the
  //          composeToFixedPoint and the C++ flattenLayer() flatten).
  // 'stage': typed-Prim Stage reconstruction (ARKit-style; heaviest).
  const bufMethod = kind === 'stage'
    ? 'exportStageAsUSDCToBufferWithOptions'
    : 'exportLayerAsUSDCToBufferWithOptions';

  if (typeof usd[bufMethod] !== 'function') {
    // Fallback: in-heap export (defeats the low-heap intent, but keeps working
    // on older glue builds).
    log(`WARN: WASM module lacks ${bufMethod}; falling back to heap-copy export.`);
    const exported = kind === 'stage'
      ? usd.exportAsUSDC()
      : (typeof usd.exportLayerAsUSDCWithOptions === 'function'
          ? usd.exportLayerAsUSDCWithOptions({ rootLayerFormat: 'usdc' })
          : null);
    if (!exported) {
      throw new Error(`USDC ${kind} export failed: ` + usd.error());
    }
    return new Uint8Array(exported);
  }

  const maxBytes = maxUSDCExportBytes(opts);
  let capacity = initialLowHeapRootCapacity(inputBytes, opts);
  if (capacity <= 0) {
    throw new Error('USDC export buffer capacity is zero.');
  }

  for (let attempt = 0; attempt < 4; attempt++) {
    const rootBuffer = new Uint8Array(capacity);
    const result = usd[bufMethod](rootBuffer, { rootLayerFormat: 'usdc' });
    if (result && result.success) {
      const size = Number(result.size || 0);
      if (size <= 0 || size > rootBuffer.length) {
        throw new Error(`USDC ${kind} export returned invalid size ${size}.`);
      }
      return rootBuffer.subarray(0, size);
    }

    const err = (result && result.error) ? result.error : usd.error();
    if (!isBufferTooSmallError(err) || capacity >= maxBytes) {
      throw new Error(`USDC ${kind} export failed: ` + err);
    }

    const nextCapacity = Math.min(maxBytes, Math.max(capacity + 1, capacity * 2));
    log(`USDC export buffer too small (${capacity} bytes); retrying with ${nextCapacity} bytes`);
    capacity = nextCapacity;
  }

  throw new Error(`USDC ${kind} export failed after repeated buffer growth.`);
}

// Back-compat alias for the layer-only callers.
function exportLayerAsUSDCOutsideWasmHeap(usd, inputBytes, opts, log) {
  return exportUSDCOutsideWasmHeap(usd, inputBytes, opts, log, 'layer');
}

// mode: 'layer' (composed PrimSpecs), 'stage' (typed-Prim, heaviest), or
// 'flatten-layer' (C++ flatten then write Layer — lightest faithful path).
async function convertSingleUSDZToLowHeapFlattenedUSDZ(native, rootPath, bytes,
                                                       opts, log, mode = 'layer') {
  const archiveEntries = parseUSDZEntries(bytes);
  const rootEntry = usdRootEntry(archiveEntries);
  if (!rootEntry) {
    throw new Error('USDZ archive contained no USD layer.');
  }

  const rootDir = rootEntry.name.includes('/')
    ? rootEntry.name.slice(0, rootEntry.name.lastIndexOf('/') + 1)
    : '';

  // The low-heap path rewrites the inner root as a top-level `root.usdc` and
  // strips the root's directory prefix when registering sublayer assets. If the
  // inner root lives in a subdirectory, that rename would re-anchor the
  // flattened root's relative asset references (textures, any remaining
  // sublayers) to the wrong archive paths. Bail out so the caller falls through
  // to the standard unpack+repack path, which keeps every entry's original full
  // path. Top-level roots (the common USDZ layout) take the low-heap path.
  if (rootDir) {
    log('Low-heap flatten skipped: inner root layer is in a subdirectory ' +
        `("${rootEntry.name}"); using standard repack to preserve asset paths.`);
    return null;
  }

  const assetNameFor = (path) =>
    (rootDir && path.startsWith(rootDir)) ? path.slice(rootDir.length) : path;

  const stats = {
    textures: archiveEntries.filter((entry) => isImageName(entry.name)).length,
    resized: 0,
    reencoded: 0,
    audio: archiveEntries.filter((entry) => isAudioName(entry.name)).length,
    otherAssets: archiveEntries.filter((entry) =>
      entry !== rootEntry && !isUsdName(entry.name) && !isImageName(entry.name) &&
      !isAudioName(entry.name)).length,
    rootPath: rootEntry.name,
    rootLayerFormat: 'usdc',
    flatten: true,
    // 'stage'/'flatten-layer' are the ARKit-style flatten paths.
    arkitCompatible: mode !== 'layer',
    lowHeapFlatten: true,
    lowHeapStage: mode === 'stage',
    lowHeapFlattenLayer: mode === 'flatten-layer',
  };

  let rootUSDC = null;
  const usd = new native.LightUSDLoaderNative();
  const guardWasmAsset = createWasmBulkAssetGuard(native, opts, log);
  try {
    if ((opts.maxUsdcMb > 0 || opts.maxMemMb > 0) &&
        typeof usd.setUSDCExportLimitMB === 'function') {
      usd.setUSDCExportLimitMB(opts.maxUsdcMb || 0, opts.maxMemMb || 0);
    }

    for (const entry of archiveEntries) {
      if (entry === rootEntry || !isUsdName(entry.name) || /\.usdz$/i.test(entry.name)) {
        continue;
      }
      const name = assetNameFor(entry.name);
      guardWasmAsset(name, entry.data, 'USD dependency layer');
      usd.setAsset(name, entry.data);
    }

    log(`Low-heap ${mode} flatten; inner root layer: ${rootEntry.name}`);
    if (!loadLayerFromBinary(usd, rootEntry.data, rootEntry.name.split('/').pop())) {
      throw new Error('Failed to load USD: ' + usd.error());
    }
    if (typeof usd.warn === 'function' && usd.warn()) log('WARN: ' + usd.warn());
    // Flatten step. 'flatten-layer' uses the single C++ flattenLayer() call
    // (lightest); the others compose at the layer level via composeToFixedPoint.
    if (mode === 'flatten-layer' && typeof usd.flattenLayer === 'function') {
      if (!usd.flattenLayer()) throw new Error('Layer flatten failed: ' + usd.error());
    } else {
      composeToFixedPoint(usd);
    }

    // Both 'layer' and 'flatten-layer' write the composed Layer (retriable);
    // only 'stage' writes the typed Stage.
    const writeKind = mode === 'stage' ? 'stage' : 'layer';
    rootUSDC = exportUSDCOutsideWasmHeap(usd, bytes.length, opts, log, writeKind);
  } finally {
    usd.delete();
  }

  const passthroughEntries = archiveEntries.filter((entry) => entry !== rootEntry);
  const usdz = buildUSDZWithNewRoot('root.usdc', rootUSDC, passthroughEntries);
  log(`Low-heap flattened USDZ wrote root.usdc (${rootUSDC.length} bytes) ` +
      `and copied ${passthroughEntries.length} entr${passthroughEntries.length === 1 ? 'y' : 'ies'}`);
  return { usdz, stats };
}

// Allocate a zero-copy input buffer and stream `usdcBytes` into the WASM heap
// (chunked, single copy). Returns the buffer uuid, or null if the allocation
// declined (e.g. exceeds the single-buffer cap) so the caller can fall back.
function coreAllocAndFill(native, usd, usdcBytes, maxBufferBytes, log) {
  const size = usdcBytes.length;
  // 0 -> wasm-side 512 MiB default; a geometry-heavy root USDC larger than that
  // raises the cap via --max-mem-mb so it streams instead of falling back.
  const info = usd.allocateZeroCopyBuffer('__core_input__', size, maxBufferBytes || 0);
  if (!info || !info.success) {
    log(`core: zero-copy alloc declined (${info && info.error}); falling back.`);
    return null;
  }
  // Re-grab HEAPU8 AFTER the allocation (it may have grown/detached the view).
  const ptr = Number(info.bufferPtr);
  const CHUNK = 16 * 1024 * 1024;
  for (let off = 0; off < size; off += CHUNK) {
    const end = Math.min(off + CHUNK, size);
    native.HEAPU8.set(usdcBytes.subarray(off, end), ptr + off);
  }
  return info.uuid;
}

// Stream a USDC buffer into the WASM heap (chunked, single copy) and flatten it
// with the core low-memory lazy-ValueRep pipeline. Returns
// { data: Uint8Array, stats } or null if the core pipeline is unavailable or
// declines (so the caller can fall back to the legacy path). `native` is the
// Emscripten Module (exposes HEAPU8); `usd` is a LightUSDLoaderNative instance.
export function flattenViaStreaming(native, usd, usdcBytes, log = () => {}, lazy = true,
                                        maxBufferBytes = 0, remap = undefined,
                                        variantSelections = undefined) {
  if (typeof usd.flattenBuffer !== 'function' ||
      typeof usd.allocateZeroCopyBuffer !== 'function') {
    return null;  // old wasm without the core pipeline
  }
  const hasRemap = remap && Object.keys(remap).length > 0;
  const hasVariants = variantSelections && Object.keys(variantSelections).length > 0;
  if (hasVariants && typeof usd.flattenBufferRemapVariants !== 'function') {
    log('core flatten variant selection requires newer wasm; falling back.');
    return null;
  }
  if (hasRemap && !hasVariants && typeof usd.flattenBufferRemap !== 'function') {
    log('core flatten asset-path remap requires newer wasm; falling back.');
    return null;
  }
  const uuid = coreAllocAndFill(native, usd, usdcBytes, maxBufferBytes, log);
  if (uuid === null) return null;
  let res = null;
  if (hasVariants) {
    res = usd.flattenBufferRemapVariants(uuid, lazy, remap, variantSelections);
  } else {
    res = hasRemap
      ? usd.flattenBufferRemap(uuid, lazy, remap)
      : usd.flattenBuffer(uuid, lazy);
  }
  if (!res || !res.success) {
    log('core flatten failed: ' + (res && res.error));
    return null;
  }
  return { data: new Uint8Array(res.data), stats: res };
}

function variantSelectionsFromOptions(opts = {}) {
  const src = opts.variantSelections || opts.variants || {};
  const out = {};
  for (const [key, value] of Object.entries(src)) {
    if (!key || value == null || String(value) === '') continue;
    out[String(key)] = String(value);
  }
  return out;
}

// Gated conversion path: flatten a single-.usdz (USDC root) with the core
// low-memory pipeline, then repack the flattened root with the original
// (unchanged) texture/audio entries. Returns { usdz, stats } or null to fall
// back to the legacy path. The core reader handles USDC only and flattens the
// single root layer (no sublayer/reference expansion yet), so non-USDC or
// nested/sublayered roots decline.
async function convertSingleUSDZToCoreLowMemUSDZ(native, bytes, opts, log) {
  const archiveEntries = parseUSDZEntries(bytes);
  const rootEntry = usdRootEntry(archiveEntries);
  if (!rootEntry) throw new Error('USDZ archive contained no USD layer.');

  if (!/\.usdc$/i.test(rootEntry.name)) {
    log(`core pipeline: root "${rootEntry.name}" is not USDC; falling back.`);
    return null;
  }
  if (rootEntry.name.includes('/')) {
    log(`core pipeline: inner root "${rootEntry.name}" is in a subdirectory; falling back.`);
    return null;
  }

  const maxBufferBytes = Number(opts.maxMemMb || 0) > 0
    ? Number(opts.maxMemMb) * 1024 * 1024 : 0;  // 0 -> wasm 512 MiB default
  const lazy = opts.eager !== true;
  const remap = textureAssetPathRemapForEntries(archiveEntries, opts.textureFormat);
  const hasRemap = Object.keys(remap).length > 0;
  const variantSelections = variantSelectionsFromOptions(opts);
  const hasVariants = Object.keys(variantSelections).length > 0;

  // Streaming-write path: the default for the core pipeline. When a patch-capable
  // (seekable) sink is available, stream the flattened root crate straight from
  // the WASM writer into the .usdz — the full output crate is never materialized
  // in the WASM heap nor copied to JS. Disable with opts.streamWrite === false
  // (CLI --no-stream-write). Without a seekable sink it transparently falls back
  // to buffering the root and repacking it. Both produce a byte-identical archive.
  const sink = opts.zipSink;
  const canStreamWrite = opts.streamWrite !== false && sink && typeof sink === 'object' &&
    typeof sink.patch === 'function' && typeof sink.write === 'function';

  const usd = new native.LightUSDLoaderNative();
  let r = null;
  let stats = null;
  try {
    if (hasRemap &&
        (typeof usd.flattenBufferRemap !== 'function' ||
         typeof usd.flattenBufferToSinkRemap !== 'function')) {
      log('core pipeline: wasm lacks asset-path remap support; falling back.');
      return null;
    }
    if (hasVariants && typeof usd.flattenBufferRemapVariants !== 'function') {
      log('core pipeline: wasm lacks variant-selection support; falling back.');
      return null;
    }
    if (hasVariants && canStreamWrite &&
        typeof usd.flattenBufferToSinkRemapVariants !== 'function') {
      log('core pipeline: wasm lacks streaming variant-selection support; falling back.');
      return null;
    }
    if (hasVariants) {
      log(`core pipeline: applying ${Object.keys(variantSelections).length} variant selection(s)`);
    }

    if (canStreamWrite && typeof usd.flattenBufferToSink === 'function') {
      // Allocate + fill BEFORE touching the sink so a declined alloc (cap) falls
      // back cleanly instead of corrupting a half-written archive.
      const uuid = coreAllocAndFill(native, usd, rootEntry.data, maxBufferBytes, log);
      if (uuid === null) return null;  // declined -> caller falls back to legacy
      r = repackUSDZEntries(native, 'root.usdc',
        { stream: (emit) => {
            const s = hasVariants && typeof usd.flattenBufferToSinkRemapVariants === 'function'
              ? usd.flattenBufferToSinkRemapVariants(uuid, lazy, (view) => { emit(view); return true; }, remap, variantSelections)
              : hasRemap && typeof usd.flattenBufferToSinkRemap === 'function'
              ? usd.flattenBufferToSinkRemap(uuid, lazy, (view) => { emit(view); return true; }, remap)
              : usd.flattenBufferToSink(uuid, lazy, (view) => { emit(view); return true; });
            if (!s || !s.success) {
              throw new Error('core stream flatten failed: ' + (s && s.error));
            }
            stats = s;
          } },
        archiveEntries, rootEntry, opts, log);
    } else {
      const flat = flattenViaStreaming(native, usd, rootEntry.data, log, lazy,
                                           maxBufferBytes, remap, variantSelections);
      if (!flat) return null;
      stats = flat.stats;
      // Repack: core-flattened root + textures (re-encoded one-at-a-time when
      // requested, else passthrough), streamed to opts.zipSink when given.
      r = repackUSDZEntries(native, 'root.usdc', flat.data, archiveEntries,
                            rootEntry, opts, log);
    }
  } finally {
    usd.delete();
  }
  if (!r || !stats) return null;

  log(`core low-mem flatten: root.usdc ${stats.inputBytes} -> ${stats.outputBytes} bytes ` +
      `(passthrough=${stats.arraysPassedThrough}, reencoded=${stats.arraysReencoded}, ` +
      `remapped=${stats.assetPathsRemapped || 0})` +
      ` read=${Number(stats.readMs || 0).toFixed(3)}ms` +
      ` compose=${Number(stats.composeMs || 0).toFixed(3)}ms` +
      ` write=${Number(stats.writeMs || 0).toFixed(3)}ms` +
      `${canStreamWrite ? ' [stream-write]' : ''}; ` +
      `repacked ${r.textures} texture(s), re-encoded ${r.reencoded}${r.streamedToSink ? ' [streamed to sink]' : ''}`);
  return {
    usdz: r.usdz,
    streamedToSink: r.streamedToSink,
    stats: {
      textures: r.textures, resized: r.resized, reencoded: r.reencoded,
      audio: r.audio, otherAssets: r.otherAssets,
      rootPath: rootEntry.name,
      rootLayerFormat: 'usdc',
      flatten: true,
      pipeline: 'core',
      arraysPassedThrough: stats.arraysPassedThrough,
      arraysReencoded: stats.arraysReencoded,
      assetPathsRemapped: stats.assetPathsRemapped || 0,
      readMs: stats.readMs,
      composeMs: stats.composeMs,
      writeMs: stats.writeMs,
    },
  };
}

// Track B: stream textures one at a time so re-encoded images never accumulate
// in the WASM heap. Flattens the root (legacy loader/writer — robust on complex
// scenes), keeping textures OUT of WASM, then re-encodes each image singly via
// native.convertImage (keeping its format, so no asset-path remap is needed) and
// repacks in JS. Peak WASM stays ~= root layer + one image, instead of all
// re-encoded textures. Returns { usdz, stats } or null to fall back to legacy.
// Applies only to a single .usdz with a top-level USD root and textureFormat
// 'keep'; format conversions (png<->jpeg) still use the legacy path (which
// rewrites the renamed asset references inside the layer).
async function convertSingleUSDZStreamTextures(native, bytes, opts, log) {
  const archiveEntries = parseUSDZEntries(bytes);
  const rootEntry = usdRootEntry(archiveEntries);
  if (!rootEntry) throw new Error('USDZ archive contained no USD layer.');
  if (rootEntry.name.includes('/')) {
    log(`stream-textures: inner root "${rootEntry.name}" in subdir; falling back.`);
    return null;
  }
  if (normalizedTextureFormat(opts.textureFormat) !== 'keep') {
    log('stream-textures: only textureFormat=keep is supported; falling back.');
    return null;
  }

  // 1) Flatten the root layer low-heap. Textures are NOT registered in WASM.
  let rootUSDC = null;
  const usd = new native.LightUSDLoaderNative();
  const guardWasmAsset = createWasmBulkAssetGuard(native, opts, log);
  try {
    if ((opts.maxUsdcMb > 0 || opts.maxMemMb > 0) &&
        typeof usd.setUSDCExportLimitMB === 'function') {
      usd.setUSDCExportLimitMB(opts.maxUsdcMb || 0, opts.maxMemMb || 0);
    }
    // Register only dependency USD layers (sublayers/refs), never images.
    for (const e of archiveEntries) {
      if (e === rootEntry || !isUsdName(e.name) || /\.usdz$/i.test(e.name)) continue;
      guardWasmAsset(e.name, e.data, 'USD dependency layer');
      usd.setAsset(e.name, e.data);
    }
    if (!loadLayerFromBinary(usd, rootEntry.data, rootEntry.name.split('/').pop())) {
      throw new Error('Failed to load USD: ' + usd.error());
    }
    const flatten = opts.flatten !== false || !!opts.arkitCompatible ||
      isMaterialOptimizationEnabled(opts) || isGeometryOptimizationEnabled(opts);
    if (flatten) {
      if (typeof usd.flattenLayer === 'function') {
        if (!usd.flattenLayer()) throw new Error('Layer flatten failed: ' + usd.error());
      } else {
        composeToFixedPoint(usd);
      }
    }
    rootUSDC = exportUSDCOutsideWasmHeap(usd, bytes.length, opts, log, 'layer');
  } finally {
    usd.delete();  // free the WASM-side layer before re-encoding textures
  }

  // 2) Repack: re-encode images one at a time (WASM holds one at a time) and
  //    stream straight to opts.zipSink when given, else build in memory.
  const rootLen = rootUSDC.length;
  const r = repackUSDZEntries(native, 'root.usdc', rootUSDC, archiveEntries,
                              rootEntry, opts, log);
  rootUSDC = null;
  log(`stream-textures: root.usdc ${rootLen} bytes, re-encoded ${r.reencoded}/${r.textures} textures (one at a time)${r.streamedToSink ? ' [streamed to sink]' : ''}`);
  return {
    usdz: r.usdz,
    streamedToSink: r.streamedToSink,
    stats: {
      textures: r.textures, resized: r.resized, reencoded: r.reencoded,
      audio: r.audio, otherAssets: r.otherAssets,
      rootPath: rootEntry.name, rootLayerFormat: 'usdc',
      flatten: true, streamTextures: true,
    },
  };
}

// opts: {
//   rootPath?, maxTextureSize?, reencode?, pngEncoder?, jpegQuality?, log?,
//   textureFormat?: "keep"|"png"|"jpeg", rootLayerFormat?: "usdc"|"usda",
//   arkitCompatible?, flatten?, textureProcessor?
// }
// returns { usdz: Uint8Array, stats: { textures, resized, reencoded, rootPath } }
async function loadTextureFeatures(native, keys, opts) {
  if (!native.loadFeature) return;
  const paths = [...keys];
  if (String(opts.textureFormat).toLowerCase() === 'exr' ||
      paths.some(path => /\.(exr|hdr)$/i.test(path)))
    await native.loadFeature('exr');
}

export async function convertFolderToUSDZ(native, assetMap, opts = {}) {
  const bake = udimOptions(opts).mode !== 'off';
  const log = opts.log || (() => {});
  const progress = typeof opts.progress === 'function' ? opts.progress : null;
  const reportProgress = (stage, current, total, message, path) => {
    if (progress) progress({ stage, current, total, message, path });
  };
  let rootPath = opts.rootPath || rootUsdFromMap(assetMap);
  if (!rootPath) throw new Error('No USD file (.usd/.usda/.usdc/.usdz) found in the input.');

  const textureFormat = normalizedTextureFormat(opts.textureFormat);
  const hasSingleUsdzInput = /\.usdz$/i.test(rootPath) && assetMap.size === 1 &&
    assetMap.has(rootPath);
  const canPassthroughUsdz = hasSingleUsdzInput &&
    opts.passthroughUsdz !== false &&
    opts.flatten === false &&
    opts.reencode === false &&
    textureFormat === 'keep' &&
    (opts.maxTextureSize || 0) <= 0 &&
    (opts.targetTextureBytes || 0) <= 0 &&
    !opts.arkitCompatible && !bake && !isMaterialOptimizationEnabled(opts) &&
    !isGeometryOptimizationEnabled(opts) && Object.keys(variantSelectionsFromOptions(opts)).length === 0 &&
    typeof opts.textureProcessor !== 'function' && typeof opts.audioProcessor !== 'function';
  if (canPassthroughUsdz) {
    const data = assetMap.get(rootPath);
    log(`Passing through USDZ unchanged: ${rootPath}`);
    reportProgress('complete', 1, 1, 'USDZ passed through unchanged', rootPath);
    return {
      usdz: data instanceof Uint8Array ? data : new Uint8Array(data),
      stats: {
        textures: 0,
        resized: 0,
        reencoded: 0,
        audio: 0,
        otherAssets: 0,
        rootPath,
        rootLayerFormat: null,
        flatten: false,
        arkitCompatible: false,
        passthrough: true,
      },
    };
  }

  if (!hasLegacyConverter(native) && hasCoreOnlyConverter(native)) {
    return convertFolderToUSDZCoreOnly(native, assetMap, opts, log, reportProgress);
  }
  await loadTextureFeatures(native,assetMap.keys(),opts);

  // Experimental: core low-memory lazy-ValueRep flatten pipeline. Opt-in via
  // opts.pipeline === 'core'. Only applies to a single .usdz with a top-level
  // USDC root (keeping textures as JS passthrough); declines (and falls back to
  // the legacy paths below) otherwise. Output fidelity is still limited (the
  // core writer drops some property types), so this is not the default.
  if (!bake && opts.pipeline === 'core' && hasSingleUsdzInput) {
    try {
      const core = await convertSingleUSDZToCoreLowMemUSDZ(
        native, assetMap.get(rootPath), opts, log);
      if (core) return core;
      log('core pipeline declined; falling back to the legacy flatten path.');
    } catch (e) {
      if (opts.zipSink) throw e;
      log('core pipeline error (' + (e && e.message) + '); falling back to legacy.');
    }
  }

  // Stream textures one at a time to bound WASM heap when re-encoding/resizing a
  // single .usdz (textureFormat 'keep'). This is the LOW-MEMORY default for that
  // case (the in-heap batch path holds every decoded image at once and OOMs the
  // wasm32 2 GB ceiling on large texture-heavy scenes). It is enabled
  // automatically; set opts.streamTextures === false (CLI: --no-stream-textures)
  // to force the in-heap path. Declines (falls back) for nested roots / non-keep
  // formats / custom texture processors.
  const wantTextureWork = (opts.maxTextureSize || 0) > 0 || opts.reencode === true;
  if (!bake && opts.streamTextures !== false && hasSingleUsdzInput &&
      textureFormat === 'keep' && wantTextureWork &&
      typeof opts.textureProcessor !== 'function') {
    try {
      const streamed = await convertSingleUSDZStreamTextures(
        native, assetMap.get(rootPath), opts, log);
      if (streamed) return streamed;
      log('stream-textures declined; falling back to the standard texture path.');
    } catch (e) {
      if (opts.zipSink) throw e;
      log('stream-textures error (' + (e && e.message) + '); falling back.');
    }
  }

  if (!bake && shouldUseLowHeapFlattenedUSDZ(rootPath, assetMap, opts, textureFormat)) {
    const lowHeap = await convertSingleUSDZToLowHeapFlattenedUSDZ(
      native, rootPath, assetMap.get(rootPath), opts, log);
    if (lowHeap) {
      return lowHeap;
    }
    // Nested-root archive: fall through to the standard unpack+repack path,
    // which preserves the full asset directory layout.
  }

  // Low-heap ARKit-style flatten path: keeps textures (no re-encode), streams
  // the flattened root USDC into a JS buffer and repacks the zip in JS, so large
  // scenes that OOM the in-heap arkit path fit. Default to 'flatten-layer' (C++
  // Layer->Layer flatten then write Layer — no typed Stage, no layer copy:
  // lighter on the wasm heap and faithful). Set opts.lowHeapStageMode='stage'
  // to force the typed-Prim Stage reconstruction instead.
  if (!bake && shouldUseLowHeapStageFlattenedUSDZ(rootPath, assetMap, opts, textureFormat)) {
    const mode = opts.lowHeapStageMode === 'stage' ? 'stage' : 'flatten-layer';
    const lowHeap = await convertSingleUSDZToLowHeapFlattenedUSDZ(
      native, rootPath, assetMap.get(rootPath), opts, log, mode);
    if (lowHeap) {
      return lowHeap;
    }
    // Nested-root archive: fall through to the standard (in-heap) path.
  }

  // If the root is a self-contained .usdz, unpack it so its internal textures
  // can be repacked (passthrough by default). Disable with repackUsdz: false.
  if (/\.usdz$/i.test(rootPath) && opts.repackUsdz !== false) {
    const expanded = expandUsdzInputs(assetMap, { log });
    if (expanded.innerRoot || [...expanded.assetMap.keys()].some(isUsdName)) {
      assetMap = expanded.assetMap;
      rootPath = expanded.innerRoot || rootUsdFromMap(assetMap);
      if (!rootPath) throw new Error('USDZ archive contained no USD layer.');
      log(`Repacking USDZ; inner root layer: ${rootPath}`);
    }
  }

  const rootLayerFormat = String(opts.rootLayerFormat || 'usdc').toLowerCase() === 'usda' ? 'usda' : 'usdc';
  const flatten = opts.flatten !== false || !!opts.arkitCompatible ||
    isMaterialOptimizationEnabled(opts) || isGeometryOptimizationEnabled(opts);

  let images = [...assetMap.keys()].filter(isImageName);
  if (/\.usdz$/i.test(rootPath)) {
    log('WARN: root is still a .usdz (could not be unpacked for texture repack); ' +
        'passing it through as an opaque layer.');
  }

  // Directory of the root USD; texture references are relative to it.
  const rootDir = rootPath.includes('/') ? rootPath.slice(0, rootPath.lastIndexOf('/') + 1) : '';
  const stats = {
    textures: 0,
    resized: 0,
    reencoded: 0,
    audio: 0,
    otherAssets: 0,
    rootPath,
    rootLayerFormat: flatten ? (opts.arkitCompatible ? 'usdc' : rootLayerFormat) : 'usda',
    flatten,
    arkitCompatible: !!opts.arkitCompatible,
  };
  if (opts.flatten === false) {
    log('WARN: non-flattened USDZ output preserves composition arcs and writes a USDA root layer.');
  }

  // USD-relative asset name for a given uploaded path.
  const assetNameFor = (path) =>
    (rootDir && path.startsWith(rootDir)) ? path.slice(rootDir.length) : path;

  const registerDependencyLayers = (usd) => {
    let current = 0;
    const total = [...assetMap.keys()].filter((path) =>
      path !== rootPath && isUsdName(path) && !/\.usdz$/i.test(path)).length;
    for (const path of assetMap.keys()) {
      if (path === rootPath || !isUsdName(path)) {
        continue;
      }
      if (/\.usdz$/i.test(path)) {
        continue;
      }
      const bytes = assetMap.get(path);
      const name = assetNameFor(path);
      guardWasmAsset(name, bytes, 'USD dependency layer');
      usd.setAsset(name, bytes);
      current++;
      reportProgress('layers', current, total, 'Registering USD dependency layers', path);
    }
  };

  // Carry through non-USD, non-image assets (audio, etc.) so they survive into
  // the repacked USDZ. Images are handled by the texture pipeline; USD layers by
  // registerDependencyLayers. Everything else is copied as-is here.
  //
  // Audio is a placeholder for a future transcode step: opts.audioProcessor, if
  // provided, may return { data, name } to replace the bytes; by default audio
  // passes through unchanged (no audio codecs are bundled in the web build).
  const registerPassthroughAssets = async (usd) => {
    const passthroughPaths = [...assetMap.keys()].filter((path) =>
      path !== rootPath && !isUsdName(path) && !isImageName(path));
    let current = 0;
    for (const path of assetMap.keys()) {
      if (path === rootPath || isUsdName(path) || isImageName(path)) {
        continue;
      }
      const name = assetNameFor(path);
      let bytes = assetMap.get(path);
      const audio = isAudioName(path);
      if (audio && typeof opts.audioProcessor === 'function') {
        try {
          const processed = await opts.audioProcessor({ path, name, data: bytes, log });
          if (processed && processed.data) bytes = new Uint8Array(processed.data);
        } catch (err) {
          log(`  ${name}: audio processing failed (${err && err.message ? err.message : err}); passing through`);
        }
      }
      guardWasmAsset(name, bytes, audio ? 'audio asset' : 'passthrough asset');
      usd.setAsset(name, bytes);
      current++;
      reportProgress('assets', current, passthroughPaths.length, 'Packaging passthrough assets', name);
      if (audio) { stats.audio++; log(`  ${name}: ${bytes.length} bytes [audio passthrough]`); }
      else { stats.otherAssets++; log(`  ${name}: ${bytes.length} bytes [asset passthrough]`); }
    }
  };

  const loadRootLayer = (usd) => {
    reportProgress('flatten', 0, 4, 'Reading root USD layer', rootPath);
    const usdBytes = assetMap.get(rootPath);
    reportProgress('flatten', 1, 4, 'Parsing root USD layer', rootPath);
    const ok = loadLayerFromBinary(usd, usdBytes, rootPath.split('/').pop());
    if (!ok) throw new Error('Failed to load USD: ' + usd.error());
    if (typeof usd.warn === 'function' && usd.warn()) log('WARN: ' + usd.warn());
    if (flatten) {
      reportProgress('flatten', 2, 4, 'Composing USD layers', rootPath);
      composeToFixedPoint(usd, (info) => {
        reportProgress(
          'flatten',
          2 + Math.min(1, Math.max(0, (info.current || 0) / (info.total || 1))),
          4,
          info.message || 'Composing USD layers',
          rootPath);
      });
    }
    reportProgress('flatten', 4, 4, 'Root layer ready', rootPath);
  };

  const usd = new native.LightUSDLoaderNative();
  const guardWasmAsset = createWasmBulkAssetGuard(native, opts, log);
  try {
    // Raise the USDC writer resource limits when requested (0 = keep the
    // conservative WASM default). Needed to export large scenes (dense meshes /
    // big timesample data) that exceed the built-in 100 MB / 256 MB caps.
    if ((opts.maxUsdcMb > 0 || opts.maxMemMb > 0) &&
        typeof usd.setUSDCExportLimitMB === 'function') {
      usd.setUSDCExportLimitMB(opts.maxUsdcMb || 0, opts.maxMemMb || 0);
    }

    let dependencyTiles = 0, dependencySets = 0, dependencyRetained = 0;
    const dependencyAtlases = [];
    if (bake && !flatten) {
      assetMap = new Map(assetMap);
      let retained = 0;
      for (const path of [...assetMap.keys()].filter(path => path !== rootPath && isUsdName(path))) {
        const dependency = new native.LightUSDLoaderNative();
        try {
          if (!loadLayerFromBinary(dependency, assetMap.get(path), path.split('/').pop()))
            throw new Error('Failed to load dependency layer: ' + dependency.error());
          const base = path.includes('/') ? path.slice(0, path.lastIndexOf('/') + 1) : '';
          const result = await bakeLayerUDIM(native, dependency,
            {keys: [...assetMap.keys()], fetch: async key => assetMap.get(key)},
            {...opts, udimMemoryBudgetBytes: udimOptions(opts).memoryBudgetBytes - retained}, base);
          const data = /\.usda$/i.test(path) ? new TextEncoder().encode(dependency.layerToString())
            : new Uint8Array(dependency.exportLayerAsUSDCWithOptions({}));
          if (!data.length) throw new Error('Failed to export baked dependency layer');
          assetMap.set(path, data);
          for (const asset of result.assets) {
            const key = base + asset.name;
            if (!assetMap.has(key)) {retained += asset.data.length; dependencyAtlases.push(key);}
            assetMap.set(key, asset.data);
          }
          dependencyTiles += result.tiles; dependencySets += result.jobs.length;
          images = images.filter(key => opts.includeUnusedTextures === true || !result.consumed.has(key));
        } finally {dependency.delete();}
      }
      images.push(...dependencyAtlases);
      dependencyRetained = retained;
    }
    registerDependencyLayers(usd);
    let baked = null;
    const bakedNames = new Set(dependencyAtlases);
    if (bake) {
      loadRootLayer(usd);
      baked = await bakeLayerUDIM(native, usd,
        {keys: [...assetMap.keys()], fetch: async key => assetMap.get(key)},
        {...opts, udimMemoryBudgetBytes: udimOptions(opts).memoryBudgetBytes - dependencyRetained}, rootDir);
      assetMap = new Map(assetMap);
      images = images.filter(path => opts.includeUnusedTextures === true || !baked.consumed.has(path));
      for (const asset of baked.assets) {
        const path = rootDir + asset.name;
        assetMap.set(path, asset.data);
        bakedNames.add(path);
        images.push(path);
      }
      stats.udimSets = baked.jobs.length + dependencySets; stats.udimTiles = baked.tiles + dependencyTiles;
    }

    // --- Budget-fit path: shrink all textures to a total byte budget. ---
    const budget = opts.targetTextureBytes || 0;
    if (budget > 0 && images.length > 0) {
      const entries = images.map((path) => ({
        path, name: assetNameFor(path), data: assetMap.get(path),
      }));
      const fit = native.fitTextures({
        images: entries.map((e) => ({ data: e.data, name: e.name })),
        targetBytes: budget,
        strategy: opts.fitStrategy === 'quality' ? 'quality' : 'size',
        startMaxSize: opts.maxTextureSize || 0,
        minTextureSize: opts.fitMinTextureSize || 64,
        minQuality: opts.fitMinQuality || 30,
        jpegQuality: opts.jpegQuality || 90,
        pngEncoder: opts.pngEncoder || 'auto',
      });
      if (!fit || !fit.success) throw new Error('fitTextures failed: ' + (fit && fit.error));
      if (fit.warn) log('WARN: ' + fit.warn);

      const remap = {};
      for (let i = 0; i < fit.results.length; i++) {
        const r = fit.results[i];
        const oldName = entries[i].name;
        const newName = replaceExt(oldName, r.ext);
        const fitBytes = new Uint8Array(r.data);
        guardWasmAsset(newName, fitBytes, 'processed texture');
        usd.setAsset(newName, fitBytes);
        stats.textures++;
        stats.reencoded++;
        if (opts.fitStrategy === 'size') stats.resized++;
        if (newName !== oldName) remap[oldName] = newName;
        reportProgress('textures', i + 1, fit.results.length, 'Fitting textures to budget', newName);
        log(`  ${oldName} -> ${newName} [${r.width}x${r.height}, ${r.data.length} bytes]`);
      }

      await registerPassthroughAssets(usd);
      if (!bake) loadRootLayer(usd);

      reportProgress('package', 0, 1, 'Writing USDZ package', rootPath);
      const data = exportUSDZ(usd, remap, opts);
      if (!data) throw new Error('USDZ export failed: ' + usd.error());
      stats.fitTotalBytes = fit.totalBytes;
      reportProgress('complete', 1, 1, 'USDZ package ready', rootPath);
      return { usdz: new Uint8Array(data), stats };
    }

    // --- Default path: per-texture resize/re-encode (preserve names unless
    // the requested output format changes the extension).
    const textureRemap = {};
    const pickResizeCs = makeResizeCsPicker(native, assetMap.get(rootPath), opts);

    // Stage A: when a textureProcessor hook is installed, run it over all
    // textures with bounded concurrency (opts.textureConcurrency) — JS/worker
    // processors parallelize across textures. The WASM fallback below stays
    // sequential (single instance), as do the setAsset calls.
    const processedByPath = new Map();
    if (typeof opts.textureProcessor === 'function' && images.length) {
      const limit = Math.min(
        normalizeTextureConcurrency(opts.textureConcurrency, 1), images.length);
      let nextImage = 0;
      let processedImages = 0;
      const runOne = async () => {
        while (nextImage < images.length) {
          const path = images[nextImage++];
          if (bakedNames.has(path)) continue;
          const bytes = assetMap.get(path);
          const assetName = (rootDir && path.startsWith(rootDir)) ? path.slice(rootDir.length) : path;
          try {
            const processed = await opts.textureProcessor({
              path,
              name: assetName,
              data: bytes,
              maxTextureSize: opts.maxTextureSize || 0,
              reencode: opts.reencode,
              textureFormat,
              jpegQuality: opts.jpegQuality || 90,
              resizeColorspace: pickResizeCs(assetName),
              log,
            });
            if (processed && processed.data) processedByPath.set(path, processed);
          } catch (err) {
            log(`  ${assetName}: texture processor failed (${err && err.message ? err.message : err}); will try WASM`);
          }
          processedImages++;
          reportProgress('textures', processedImages, images.length, 'Pre-processing textures', assetName);
        }
      };
      await Promise.all(Array.from({ length: limit }, runOne));
    }

    let packagedTextures = 0;
    for (const path of images) {
      const bytes = assetMap.get(path);
      // Name as the USD most likely references it (relative to the USD's dir).
      let assetName = (rootDir && path.startsWith(rootDir)) ? path.slice(rootDir.length) : path;

      stats.textures++;
      // The WASM convertImage fallback below is synchronous; yield to the
      // event loop periodically so a driving page/CLI can surface progress
      // during multi-minute texture phases.
      if ((stats.textures & 7) === 0) await new Promise((r) => setTimeout(r, 0));
      let outBytes = bytes;

      const fmtInfo = outputFormatForImage(path, textureFormat);
      const wantResize = (opts.maxTextureSize || 0) > 0;
      const processed = processedByPath.get(path) || null;

      if (processed && processed.data) {
        outBytes = new Uint8Array(processed.data);
        if (processed.name && processed.name !== assetName) {
          textureRemap[assetName] = processed.name;
          assetName = processed.name;
        } else if (processed.ext) {
          const newName = replaceExt(assetName, processed.ext);
          if (newName !== assetName) textureRemap[assetName] = newName;
          assetName = newName;
        }
        if (processed.resized) stats.resized++;
        if (processed.reencoded || outBytes !== bytes) stats.reencoded++;
        log(`  ${assetName}: ${bytes.length} -> ${outBytes.length} bytes [browser]`);
      } else if (!bakedNames.has(path) && fmtInfo.format && (wantResize || opts.reencode || textureFormat !== 'keep')) {
        const res = native.convertImage(bytes, {
          maxSize: opts.maxTextureSize || 0,
          format: fmtInfo.format,
          pngEncoder: opts.pngEncoder || 'auto',
          jpegQuality: opts.jpegQuality || 90,
          resizeColorspace: pickResizeCs(assetName),
        });
        if (res && res.success) {
          outBytes = new Uint8Array(res.data); // copy out of wasm heap
          if (fmtInfo.ext) {
            const newName = replaceExt(assetName, fmtInfo.ext);
            if (newName !== assetName) textureRemap[assetName] = newName;
            assetName = newName;
          }
          if (res.resized) stats.resized++;
          stats.reencoded++;
          log(`  ${assetName}: ${bytes.length} -> ${outBytes.length} bytes` +
              (res.resized ? ` [resized ${res.width}x${res.height}]` : ' [reencoded]'));
        } else {
          log(`  ${assetName}: convertImage failed (${res && res.error}); using original`);
        }
      } else {
        log(`  ${assetName}: ${bytes.length} bytes [passthrough]`);
      }

      guardWasmAsset(assetName, outBytes, 'texture');
      usd.setAsset(assetName, outBytes);
      packagedTextures++;
      reportProgress('textures', packagedTextures, images.length, 'Packaging textures', assetName);
    }

    await registerPassthroughAssets(usd);
    if (!bake) loadRootLayer(usd);


    reportProgress('package', 0, 1, 'Writing USDZ package', rootPath);
    const data = exportUSDZ(usd, textureRemap, opts);
    if (!data) throw new Error('USDZ export failed: ' + usd.error());
    const usdz = new Uint8Array(data); // copy out of wasm heap

    reportProgress('complete', 1, 1, 'USDZ package ready', rootPath);
    return { usdz, stats };
  } finally {
    usd.delete();
  }
}

// ---------------------------------------------------------------------------
// Streaming folder/URL-list -> USDZ conversion.
//
// Unlike convertFolderToUSDZ (which takes the whole file set as an in-memory
// Map: scene bytes + every texture + every processed output live concurrently
// in JS and in the wasm asset cache), this path fetches lazily from a `source`:
//
//   source = { keys: string[],                    // relative asset paths
//              fetch: async (key) => Uint8Array } // file / HTTP fetch
//
// Only the USD layers (small) enter the wasm resolver cache for composition;
// textures are fetched -> processed -> appended to the USDZ zip sink ->
// released one at a time (bounded prefetch), so peak RSS is roughly the
// composition heap plus a handful of textures instead of the entire scene.
//
// Requires flatten (the streamed root inlines dependency layers). Texture
// renames (e.g. --texture-format jpeg on .png inputs) are precomputed from the
// name/format alone and applied to the composed layer via
// usd.remapLayerAssetPaths() BEFORE the root is written (the root must be the
// first zip entry, so the remap cannot wait for texture processing).
// ---------------------------------------------------------------------------
export async function convertSourceToUSDZStreaming(native, source, opts = {}) {
  const bake = udimOptions(opts).mode !== 'off';
  const log = opts.log || (() => {});
  const progress = typeof opts.progress === 'function' ? opts.progress : null;
  const reportProgress = (stage, current, total, message, path) => {
    if (progress) progress({ stage, current, total, message, path });
  };
  const keys = source.keys;
  if (!keys || !keys.length) throw new Error('streaming source has no files');
  if (opts.flatten === false && hasLegacyConverter(native)) {
    throw new Error('streaming conversion requires flatten (composition arcs cannot reference streamed entries)');
  }

  // Root selection: reuse rootUsdFromMap over a keys-only Map.
  const keyMap = new Map(keys.map((k) => [k, null]));
  const rootPath = opts.rootPath || rootUsdFromMap(keyMap);
  if (!rootPath) throw new Error('No USD file (.usd/.usda/.usdc) found in the input.');
  if (/\.usdz$/i.test(rootPath)) {
    throw new Error('streaming conversion takes a scene folder/url-list, not a packed .usdz root');
  }

  if (!hasLegacyConverter(native) && hasCoreOnlyConverter(native)) {
    return convertSourceToUSDZCoreOnly(native, source, opts, rootPath);
  }
  await loadTextureFeatures(native,source.keys || [],opts);

  const textureFormat = normalizedTextureFormat(opts.textureFormat);
  const rootDir = rootPath.includes('/') ? rootPath.slice(0, rootPath.lastIndexOf('/') + 1) : '';
  const assetNameFor = (p) => (rootDir && p.startsWith(rootDir)) ? p.slice(rootDir.length) : p;
  const normalizeArchiveAssetName = (p) => assetNameFor(String(p || '').replace(/\\/g, '/'))
    .replace(/^\.\/+/, '').replace(/^\/+/, '');

  const stats = {
    textures: 0, resized: 0, reencoded: 0, audio: 0, otherAssets: 0,
    rootPath, rootLayerFormat: 'usdc', flatten: true,
    arkitCompatible: false, streaming: true,
  };

  const usd = new native.LightUSDLoaderNative();
  const guardWasmAsset = createWasmBulkAssetGuard(native, opts, log);
  try {
    if ((opts.maxUsdcMb > 0 || opts.maxMemMb > 0) &&
        typeof usd.setUSDCExportLimitMB === 'function') {
      usd.setUSDCExportLimitMB(opts.maxUsdcMb || 0, opts.maxMemMb || 0);
    }

    // 1. USD layers only into the wasm resolver cache (composition needs them;
    //    they are small relative to textures). Node/folder sources may provide a
    //    synchronous fetch hook; in that case the core path can load dependency
    //    layers on demand and only preloads the root here.
    const usdKeys = keys.filter((k) => isUsdName(k) && !/\.usdz$/i.test(k));
    const usdKeyByAssetName = new Map();
    for (const key of usdKeys) {
      usdKeyByAssetName.set(normalizeArchiveAssetName(key), key);
    }
    const canFetchUsdLayerSync =
      !bake && opts.pipeline === 'core' &&
      typeof source.fetchSync === 'function' &&
      typeof usd.flattenMultiBufferToSinkFetch === 'function';
    const canFetchUsdLayerAsync =
      !bake && opts.pipeline === 'core' &&
      opts.preloadUsdLayers !== true &&
      !canFetchUsdLayerSync &&
      typeof usd.flattenAsyncBegin === 'function' &&
      typeof usd.flattenAsyncProvideLayer === 'function' &&
      typeof usd.flattenAsyncStep === 'function' &&
      typeof usd.flattenAsyncEnd === 'function';
    const resolveUsdKey = (name) => {
      const norm = normalizeArchiveAssetName(name);
      if (usdKeyByAssetName.has(norm)) return usdKeyByAssetName.get(norm);
      for (const [assetName, key] of usdKeyByAssetName) {
        if (assetName.endsWith('/' + norm) || norm.endsWith('/' + assetName)) {
          return key;
        }
      }
      return null;
    };
    let usdBytesTotal = 0;
    let usdLayersPreloaded = 0;
    let rootBytes = null;
    const preloadedUsdLayerBytes = new Map();
    const rememberPreloadedUsdLayer = (key, bytes) => {
      preloadedUsdLayerBytes.set(key, bytes);
      preloadedUsdLayerBytes.set(assetNameFor(key), bytes);
    };
    const preloadUsdDependencies = async () => {
      for (const key of usdKeys) {
        if (key === rootPath) continue;
        // eslint-disable-core-line no-await-in-loop
        const bytes = await source.fetch(key);
        usdBytesTotal += bytes.length;
        usdLayersPreloaded++;
        rememberPreloadedUsdLayer(key, bytes);
        const name = assetNameFor(key);
        guardWasmAsset(name, bytes, 'USD dependency layer');
        usd.setAsset(name, bytes);
        reportProgress('layers', usdLayersPreloaded, usdKeys.length, 'Preloading USD dependency layers', key);
      }
    };
    for (const key of usdKeys) {
      if ((canFetchUsdLayerSync || canFetchUsdLayerAsync) && key !== rootPath) continue;
      // eslint-disable-core-line no-await-in-loop
      const bytes = await source.fetch(key);
      usdBytesTotal += bytes.length;
      usdLayersPreloaded++;
      rememberPreloadedUsdLayer(key, bytes);
      if (key === rootPath) rootBytes = bytes;
      else {
        const name = assetNameFor(key);
        guardWasmAsset(name, bytes, 'USD dependency layer');
        usd.setAsset(name, bytes);
      }
      reportProgress('layers', usdLayersPreloaded, usdKeys.length, 'Reading USD layers', key);
    }
    if (!rootBytes) throw new Error(`root ${rootPath} not fetchable from source`);
    const canProvidePreloadedUsdLayerSync =
      !bake && opts.pipeline === 'core' &&
      opts.preloadUsdLayers === true &&
      !canFetchUsdLayerSync &&
      typeof usd.flattenMultiBufferToSinkFetch === 'function';
    const canProvideUsdLayerSync = canFetchUsdLayerSync || canProvidePreloadedUsdLayerSync;
    const getUsdLayerSync = (key) => {
      if (!key) return null;
      if (typeof source.fetchSync === 'function') return source.fetchSync(key);
      return preloadedUsdLayerBytes.get(key) || preloadedUsdLayerBytes.get(assetNameFor(key)) || null;
    };
    if (canFetchUsdLayerSync || canFetchUsdLayerAsync) {
      log(`streaming: ${usdKeys.length} USD layer(s), ${usdLayersPreloaded} preloaded ` +
          `(${(usdBytesTotal / 1e6).toFixed(1)} MB); dependency layers load on demand; textures stream on demand`);
    } else if (canProvidePreloadedUsdLayerSync) {
      log(`streaming: ${usdKeys.length} USD layer(s), ${(usdBytesTotal / 1e6).toFixed(1)} MB preloaded; ` +
          `core dependency callbacks use preloaded layers; textures stream on demand`);
    } else {
      log(`streaming: ${usdKeys.length} USD layer(s), ${(usdBytesTotal / 1e6).toFixed(1)} MB in cache; textures stream on demand`);
    }

    // 1.5 Opt-in core multi-asset pipeline: compose/flatten/write happen inside
    // one wasm call (flattenMultiBufferToSink) with external arcs resolved
    // against the asset cache the loop above just fed. Dependency layers are
    // consumed from the cache as they load and arrays pass through verbatim,
    // so the heap stays near the input bytes instead of the legacy composed +
    // re-encoded copies. Texture rename remaps are passed into the core flatten
    // call and applied after composition before the root crate is streamed.
    let coreRoot = null;  // { uuid, anchor } when the core pipeline is armed
    if (!bake && opts.pipeline === 'core') {
      const rootIsUSDC = rootBytes.length >= 8 &&
        rootBytes[0] === 0x50 && rootBytes[1] === 0x58 && rootBytes[2] === 0x52 &&
        rootBytes[3] === 0x2d && rootBytes[4] === 0x55 && rootBytes[5] === 0x53 &&
        rootBytes[6] === 0x44 && rootBytes[7] === 0x43;  // "PXR-USDC"
      if (rootIsUSDC && typeof usd.flattenMultiBufferToSink === 'function') {
        const maxBufferBytes = Number(opts.maxMemMb || 0) > 0
          ? Number(opts.maxMemMb) * 1024 * 1024 : 0;
        const uuid = coreAllocAndFill(native, usd, rootBytes, maxBufferBytes, log);
        if (uuid !== null) {
          coreRoot = {
            uuid,
            anchor: assetNameFor(rootPath),
            syncLayers: canProvideUsdLayerSync,
            asyncLayers: canFetchUsdLayerAsync,
          };
        }
        else log('core: zero-copy alloc declined; using the legacy streaming path.');
      } else {
        log('core pipeline unavailable for this input (non-USDC root or old wasm); ' +
            'using the legacy streaming path.');
      }
    }

    // 2. Compose/flatten (legacy path; skipped when the core pipeline is armed —
    //    it composes inside the flatten call at step 4/5).
    if (!coreRoot) {
      if (canFetchUsdLayerSync) {
        await preloadUsdDependencies();
      }
      reportProgress('flatten', 0, 4, 'Root USD bytes ready', rootPath);
      reportProgress('flatten', 1, 4, 'Parsing root USD layer', rootPath);
      if (!loadLayerFromBinary(usd, rootBytes, rootPath.split('/').pop())) {
        throw new Error('Failed to load USD: ' + usd.error());
      }
      reportProgress('flatten', 2, 4, 'Composing USD layers', rootPath);
      composeToFixedPoint(usd, (info) => {
        reportProgress(
          'flatten',
          2 + Math.min(1, Math.max(0, (info.current || 0) / (info.total || 1))),
          4,
          info.message || 'Composing USD layers',
          rootPath);
      });
      reportProgress('flatten', 4, 4, 'Root layer ready', rootPath);
      // Composition is done: drop the previous-iteration source layer (a full
      // copy of the composed scene) and the USD-layer bytes in the wasm asset
      // cache — only the composed layer is used from here on.
      if (typeof usd.releaseSourceLayer === 'function') usd.releaseSourceLayer();
      if (typeof usd.clearAssets === 'function') usd.clearAssets();
    }

    const baked = bake ? await bakeLayerUDIM(native, usd, source, opts, rootDir, false) : null;
    if (baked) { stats.udimSets = baked.jobs.length; stats.udimTiles = baked.tiles; }

    // 3. Precompute texture output names; remap renamed references in the
    //    composed layer before the root is written.
    const images = keys.filter(key => isImageName(key) && (!baked || opts.includeUnusedTextures === true || !baked.consumed.has(key)));
    const plans = images.map((path) => {
      const name = assetNameFor(path);
      const fmtInfo = outputFormatForImage(path, textureFormat);
      let outName = name;
      if (fmtInfo.format && fmtInfo.ext) {
        const renamed = replaceExt(name, fmtInfo.ext);
        if (renamed !== name) outName = renamed;
      }
      return { path, name, outName, format: fmtInfo.format };
    });
    const remap = {};
    for (const p of plans) {
      if (p.outName !== p.name) addTextureRemapAliases(remap, p.name, p.outName);
    }
    const hasRemap = Object.keys(remap).length > 0;
    const variantSelections = variantSelectionsFromOptions(opts);
    const hasVariants = Object.keys(variantSelections).length > 0;
    if (hasRemap && !coreRoot) {
      if (typeof usd.remapLayerAssetPaths !== 'function') {
        throw new Error('texture format change requires remapLayerAssetPaths (rebuild the wasm module), or use --texture-format keep');
      }
      const n = usd.remapLayerAssetPaths(remap);
      log(`streaming: remapped ${n} texture reference(s) for format change`);
    } else if (hasRemap && coreRoot) {
      log(`streaming: queued ${Object.keys(remap).length} texture path rename(s) for core flatten`);
    }
    if (hasVariants && coreRoot) {
      log(`streaming: applying ${Object.keys(variantSelections).length} variant selection(s) in core flatten`);
    }

    let referencedAssetNames = null;
    let referencedAssetList = [];
    const isReferencedArchiveName = (name) => {
      if (!referencedAssetNames) return true;
      const norm = normalizeArchiveAssetName(name);
      if (referencedAssetNames.has(norm)) return true;
      return referencedAssetList.some((ref) =>
        ref.endsWith('/' + norm) || norm.endsWith('/' + ref));
    };
    const referencedArchiveNameFor = (name) => {
      if (!referencedAssetNames) return name;
      const norm = normalizeArchiveAssetName(name);
      if (referencedAssetNames.has(norm)) return norm;
      const ref = referencedAssetList.find((candidate) =>
        candidate.endsWith('/' + norm) || norm.endsWith('/' + candidate));
      return ref || name;
    };

    // 4. Export the flattened root as a bare USDC buffer (JS-side zip). The
    //    flattened root inlines every dependency layer, so size it from the
    //    total USD bytes, not the root file alone. (The core pipeline instead
    //    flattens+writes inside one wasm call at step 5, streamed when the
    //    sink is patch-capable.)
    const rootName = rootPath.split('/').pop().replace(/\.(usda|usd)$/i, '.usdc');
    if (!coreRoot) {
      reportProgress('package', 0, 1, 'Writing flattened root layer', rootName);
    }
    const rootOut = coreRoot
      ? null : exportUSDCOutsideWasmHeap(usd, usdBytesTotal, opts, log, 'layer');
    if (!coreRoot) {
      log(`streaming: composed root exported; wasm heap ` +
          `${Math.round((native.HEAPU8 ? native.HEAPU8.length : 0) / 1048576)} MiB`);
    }

    // 5. Stream the zip: root first, then textures one at a time.
    const chunks = [];
    const sink = opts.zipSink || ((bytes) => { chunks.push(bytes.slice ? bytes.slice() : new Uint8Array(bytes)); });
    const zw = new ZipStreamWriter(sink);
    if (coreRoot) {
      const lazy = opts.eager !== true;
      const canStreamWrite = opts.streamWrite !== false && zw.canStream();
      if (hasRemap) {
        const hasRemapBinding = coreRoot.asyncLayers
          ? typeof usd.flattenAsyncBeginRemap === 'function'
          : typeof usd.flattenMultiBufferToSinkFetchRemap === 'function';
        if (!hasRemapBinding) {
          throw new Error('core pipeline texture format change requires remap-capable wasm bindings');
        }
      }
      if (hasVariants) {
        const hasVariantBinding = coreRoot.asyncLayers
          ? typeof usd.flattenAsyncBeginRemapVariants === 'function'
          : typeof usd.flattenMultiBufferToSinkFetchRemapVariants === 'function';
        if (!hasVariantBinding) {
          throw new Error('core pipeline variant selection requires variant-capable wasm bindings');
        }
      }
      let s = null;
      let asyncSession = null;
      if (coreRoot.asyncLayers) {
        reportProgress('flatten', 0, 1, 'Preparing core flatten pipeline', rootPath);
        const begin = hasVariants && typeof usd.flattenAsyncBeginRemapVariants === 'function'
          ? usd.flattenAsyncBeginRemapVariants(coreRoot.uuid, coreRoot.anchor, lazy, remap, variantSelections)
          : hasRemap && typeof usd.flattenAsyncBeginRemap === 'function'
          ? usd.flattenAsyncBeginRemap(coreRoot.uuid, coreRoot.anchor, lazy, remap)
          : usd.flattenAsyncBegin(coreRoot.uuid, coreRoot.anchor, lazy);
        if (!begin || !begin.success) {
          throw new Error('core async flatten begin failed: ' + (begin && begin.error));
        }
        asyncSession = begin.session;
        for (;;) {
          const r = usd.flattenAsyncStep(asyncSession, () => false);
          if (!r || !r.success) {
            throw new Error('core async flatten preflight failed: ' + (r && r.error));
          }
          if (r.status === 'need-layer') {
            const key = resolveUsdKey(r.key);
            if (!key) throw new Error(`core async flatten requested unknown layer: ${r.key}`);
            // eslint-disable-core-line no-await-in-loop
            const bytes = await source.fetch(key);
            const pr = usd.flattenAsyncProvideLayer(asyncSession, r.key, bytes);
            if (!pr || !pr.success) {
              throw new Error('core async flatten provide failed: ' + (pr && pr.error));
            }
            reportProgress('layers', usdLayersPreloaded, usdKeys.length, 'Providing USD dependency layer', key);
            continue;
          }
          if (r.status === 'ready' || r.status === 'done') break;
          throw new Error(`core async flatten unexpected status: ${r.status}`);
        }
      }
      if (canStreamWrite) {
        // Once streaming starts a failure corrupts the archive, so errors
        // throw rather than fall back (same contract as the single-usdz path).
        reportProgress('flatten', 0, 1, 'Writing flattened root layer', rootName);
        zw.addEntryStreaming(rootName, (emit) => {
          const emitCb = (view) => { emit(view); return true; };
          if (asyncSession) {
            s = usd.flattenAsyncStep(asyncSession, emitCb);
            usd.flattenAsyncEnd(asyncSession);
          } else if (coreRoot.syncLayers &&
              typeof usd.flattenMultiBufferToSinkFetch === 'function') {
            const hasLayer = (name) =>
              resolveUsdKey(name) !== null;
            const fetchLayer = (name) => {
              const key = resolveUsdKey(name);
              return getUsdLayerSync(key);
            };
            if (hasVariants && typeof usd.flattenMultiBufferToSinkFetchRemapVariants === 'function') {
              s = usd.flattenMultiBufferToSinkFetchRemapVariants(
                coreRoot.uuid, coreRoot.anchor, lazy, emitCb, hasLayer, fetchLayer, remap, variantSelections);
            } else if (hasRemap && typeof usd.flattenMultiBufferToSinkFetchRemap === 'function') {
              s = usd.flattenMultiBufferToSinkFetchRemap(
                coreRoot.uuid, coreRoot.anchor, lazy, emitCb, hasLayer, fetchLayer, remap);
            } else {
              s = usd.flattenMultiBufferToSinkFetch(
                coreRoot.uuid, coreRoot.anchor, lazy, emitCb, hasLayer, fetchLayer);
            }
          } else {
            if (hasVariants && typeof usd.flattenMultiBufferToSinkFetchRemapVariants === 'function') {
              s = usd.flattenMultiBufferToSinkFetchRemapVariants(
                coreRoot.uuid, coreRoot.anchor, lazy, emitCb, undefined, undefined, remap, variantSelections);
            } else if (hasRemap && typeof usd.flattenMultiBufferToSinkFetchRemap === 'function') {
              s = usd.flattenMultiBufferToSinkFetchRemap(
                coreRoot.uuid, coreRoot.anchor, lazy, emitCb, undefined, undefined, remap);
            } else {
              s = usd.flattenMultiBufferToSink(
                coreRoot.uuid, coreRoot.anchor, lazy, emitCb);
            }
          }
          if (!s || !s.success || (s.status && s.status !== 'done')) {
            throw new Error('core multi flatten failed: ' + (s && s.error));
          }
        });
      } else {
        reportProgress('flatten', 0, 1, 'Writing flattened root layer', rootName);
        if (asyncSession) {
          s = usd.flattenAsyncStep(asyncSession, null);
          usd.flattenAsyncEnd(asyncSession);
        } else if (coreRoot.syncLayers &&
            typeof usd.flattenMultiBufferToSinkFetch === 'function') {
          const hasLayer = (name) =>
            resolveUsdKey(name) !== null;
          const fetchLayer = (name) => {
            const key = resolveUsdKey(name);
            return getUsdLayerSync(key);
          };
          if (hasVariants && typeof usd.flattenMultiBufferToSinkFetchRemapVariants === 'function') {
            s = usd.flattenMultiBufferToSinkFetchRemapVariants(
              coreRoot.uuid, coreRoot.anchor, lazy, null, hasLayer, fetchLayer, remap, variantSelections);
          } else if (hasRemap && typeof usd.flattenMultiBufferToSinkFetchRemap === 'function') {
            s = usd.flattenMultiBufferToSinkFetchRemap(
              coreRoot.uuid, coreRoot.anchor, lazy, null, hasLayer, fetchLayer, remap);
          } else {
            s = usd.flattenMultiBufferToSinkFetch(
              coreRoot.uuid, coreRoot.anchor, lazy, null, hasLayer, fetchLayer);
          }
        } else {
          if (hasVariants && typeof usd.flattenMultiBufferToSinkFetchRemapVariants === 'function') {
            s = usd.flattenMultiBufferToSinkFetchRemapVariants(
              coreRoot.uuid, coreRoot.anchor, lazy, null, undefined, undefined, remap, variantSelections);
          } else if (hasRemap && typeof usd.flattenMultiBufferToSinkFetchRemap === 'function') {
            s = usd.flattenMultiBufferToSinkFetchRemap(
              coreRoot.uuid, coreRoot.anchor, lazy, null, undefined, undefined, remap);
          } else {
            s = usd.flattenMultiBufferToSink(coreRoot.uuid, coreRoot.anchor, lazy, null);
          }
        }
        if (!s || !s.success || (s.status && s.status !== 'done')) {
          throw new Error('core multi flatten failed: ' + (s && s.error));
        }
        zw.addEntry(rootName, new Uint8Array(s.data));
      }
      reportProgress('flatten', 1, 1, 'Root layer ready', rootName);
      if (typeof usd.clearAssets === 'function') usd.clearAssets();
      stats.pipeline = 'core';
      stats.arraysPassedThrough = s.arraysPassedThrough;
      stats.arraysReencoded = s.arraysReencoded;
      stats.assetPathsRemapped = s.assetPathsRemapped || 0;
      stats.readMs = s.readMs;
      stats.composeMs = s.composeMs;
      stats.writeMs = s.writeMs;
      if (Array.isArray(s.assetPaths) && s.assetPaths.length > 0) {
        referencedAssetList = s.assetPaths
          .map((p) => normalizeArchiveAssetName(p))
          .filter((p) => p.length > 0);
        referencedAssetNames = new Set(referencedAssetList);
        stats.referencedAssets = referencedAssetList.length;
      }
      log(`core multi flatten: ${rootName} ${s.inputBytes} -> ${s.outputBytes} bytes, ` +
          `${s.primCount} prims (passthrough=${s.arraysPassedThrough}, ` +
          `reencoded=${s.arraysReencoded}, remapped=${stats.assetPathsRemapped}) ` +
          `read=${Number(s.readMs || 0).toFixed(3)}ms ` +
          `compose=${Number(s.composeMs || 0).toFixed(3)}ms ` +
          `write=${Number(s.writeMs || 0).toFixed(3)}ms` +
          `${canStreamWrite ? ' [stream-write]' : ''}; ` +
          `wasm heap ${Math.round((native.HEAPU8 ? native.HEAPU8.length : 0) / 1048576)} MiB`);
      if (s.compositionErrorCount > 0) {
        log(`core multi flatten: ${s.compositionErrorCount} composition error(s) ` +
            '(arcs skipped):');
        for (const e of s.compositionErrors || []) log(`  ${e}`);
      }
    } else {
      zw.addEntry(rootName, rootOut);
    }

    // No encoded atlases are retained during root preparation. Replay each
    // validated bake after root emission, keeping only one atlas in memory.
    if (baked) for (const job of baked.jobs) {
      const atlas = await bakeUDIMAtlas(native, source, job.site.pattern, {...opts, udimSharedLayout: job.sharedLayout}, rootDir, job.site.srgb);
      if (atlas.name !== job.digestName) throw new Error('UDIM source changed during streaming conversion');
      zw.addEntry(job.name, atlas.data); stats.textures++;
    }

    const pickResizeCs = makeResizeCsPicker(native, rootBytes, opts);
    const wantResize = (opts.maxTextureSize || 0) > 0;
    const wantWork = (fmt) => fmt && (wantResize || opts.reencode !== false || textureFormat !== 'keep');

    // Bounded prefetch pipeline: fetch/process up to `width` textures ahead,
    // append to the zip in order so at most `width` outputs are in memory.
    const width = normalizeTextureConcurrency(opts.textureConcurrency, 4);
    const includeUnusedTextures = opts.includeUnusedTextures === true;
    const texturePlans = referencedAssetNames && !includeUnusedTextures
      ? plans.filter((plan) =>
          isReferencedArchiveName(plan.name) ||
          isReferencedArchiveName(plan.outName))
      : plans;
    if (referencedAssetNames && includeUnusedTextures) {
      const unreferencedCount = plans.filter((plan) =>
        !isReferencedArchiveName(plan.name) &&
        !isReferencedArchiveName(plan.outName)).length;
      stats.unusedTexturesIncluded = unreferencedCount;
      if (unreferencedCount > 0) {
        log(`streaming: including ${unreferencedCount} unreferenced texture asset(s) by request`);
      }
    } else if (referencedAssetNames && texturePlans.length !== plans.length) {
      stats.unreferencedTexturesSkipped = plans.length - texturePlans.length;
      log(`streaming: skipped ${plans.length - texturePlans.length} unreferenced texture asset(s)`);
    }
    const processOne = async (plan) => {
      let bytes;
      try {
        bytes = await source.fetch(plan.path);
      } catch (err) {
        // Resolve to a sentinel — never reject. This promise is pre-launched by
        // the bounded-prefetch pump but awaited FIFO, so a non-head rejection
        // would sit unhandled until the loop reaches it, tripping Node's default
        // unhandledRejection policy and killing the process. The consume loop
        // turns this sentinel into a clean error instead.
        return { error: err };
      }
      let outBytes = bytes;
      let resized = false, reencoded = false;
      if (typeof opts.textureProcessor === 'function') {
        try {
          const processed = await opts.textureProcessor({
            path: plan.path, name: plan.name, data: bytes,
            maxTextureSize: opts.maxTextureSize || 0,
            reencode: opts.reencode, textureFormat,
            jpegQuality: opts.jpegQuality || 90,
            resizeColorspace: pickResizeCs(plan.name), log,
          });
          if (processed && processed.data) {
            outBytes = new Uint8Array(processed.data);
            resized = !!processed.resized;
            reencoded = true;
          }
        } catch (err) {
          log(`  ${plan.name}: texture processor failed (${err && err.message ? err.message : err}); trying WASM`);
        }
      }
      if (outBytes === bytes && wantWork(plan.format)) {
        let res = null;
        try {
          res = native.convertImage(bytes, {
            maxSize: opts.maxTextureSize || 0,
            format: plan.format,
            pngEncoder: opts.pngEncoder || 'auto',
            jpegQuality: opts.jpegQuality || 90,
            resizeColorspace: pickResizeCs(plan.name),
          });
        } catch (err) {
          // A synchronous throw here would reject this pre-launched promise the
          // same way a failed fetch does; treat it like a convertImage failure
          // and pass the original through.
          log(`  ${plan.name}: convertImage threw (${err && err.message ? err.message : err}); passing original through`);
        }
        if (res && res.success) {
          outBytes = new Uint8Array(res.data);
          resized = !!res.resized;
          reencoded = true;
        } else if (res) {
          log(`  ${plan.name}: convertImage failed (${res.error}); passing original through`);
        }
      }
      return { outBytes, resized, reencoded };
    };

    const inflight = [];
    let planIdx = 0;
    const pump = () => {
      while (planIdx < texturePlans.length && inflight.length < width) {
        const plan = texturePlans[planIdx++];
        inflight.push({ plan, promise: processOne(plan) });
      }
    };
    pump();
    while (inflight.length) {
      const { plan, promise } = inflight.shift();
      // eslint-disable-core-line no-await-in-loop
      const r = await promise;
      if (r.error) {
        // A referenced texture could not be fetched — fail cleanly rather than
        // emit an archive missing the asset. Any remaining in-flight jobs resolve
        // to sentinels (they never reject), so abandoning them here is safe.
        throw new Error(`streaming: failed to fetch texture '${plan.path}': ${r.error && r.error.message ? r.error.message : r.error}`);
      }
      const { outBytes, resized, reencoded } = r;
      const archiveName = referencedArchiveNameFor(plan.outName);
      zw.addEntry(archiveName, outBytes);
      stats.textures++;
      if (resized) stats.resized++;
      if (reencoded) stats.reencoded++;
      reportProgress('textures', stats.textures, texturePlans.length, 'Streaming textures into USDZ', archiveName);
      log(`  ${archiveName}: ${outBytes.length} bytes${resized ? ' [resized]' : reencoded ? ' [reencoded]' : ' [passthrough]'}`);
      pump();
    }

    // 6. Non-USD, non-image assets (audio etc.) pass through, also streamed.
    const passthroughKeys = keys.filter((key) =>
      key !== rootPath && !isUsdName(key) && !isImageName(key) &&
      (!referencedAssetNames || isReferencedArchiveName(assetNameFor(key))));
    let passthroughCount = 0;
    for (const key of keys) {
      if (key === rootPath || isUsdName(key) || isImageName(key)) continue;
      if (referencedAssetNames && !isReferencedArchiveName(assetNameFor(key))) continue;
      // eslint-disable-core-line no-await-in-loop
      const bytes = await source.fetch(key);
      zw.addEntry(assetNameFor(key), bytes);
      passthroughCount++;
      reportProgress('assets', passthroughCount, passthroughKeys.length, 'Streaming passthrough assets', assetNameFor(key));
      if (isAudioName(key)) stats.audio++; else stats.otherAssets++;
    }

    reportProgress('package', 1, 1, 'Finalizing USDZ package', rootName);
    zw.finalize();

    if (opts.zipSink) {
      return { usdz: null, streamedToSink: true, stats };
    }
    let total = 0;
    for (const c of chunks) total += c.length;
    const usdz = new Uint8Array(total);
    let pos = 0;
    for (const c of chunks) { usdz.set(c, pos); pos += c.length; }
    reportProgress('complete', 1, 1, 'USDZ package ready', rootName);
    return { usdz, streamedToSink: false, stats };
  } finally {
    usd.delete();
  }
}
