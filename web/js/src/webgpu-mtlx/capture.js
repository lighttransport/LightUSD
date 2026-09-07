// SPDX-License-Identifier: Apache-2.0
// OpenEXR v2, uncompressed scanline RGB FLOAT. No lossy/tone-mapped reference data.
export function encodeEXR(width, height, rgba) {
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || rgba.length !== width * height * 4) throw new Error('Invalid EXR dimensions');
  const bytes = [], encoder = new TextEncoder();
  const str = s => [...encoder.encode(s), 0];
  const int = v => { const a = new Uint8Array(4); new DataView(a.buffer).setInt32(0, v, true); return [...a]; };
  const float = v => { const a = new Uint8Array(4); new DataView(a.buffer).setFloat32(0, v, true); return [...a]; };
  const attr = (name, type, value) => bytes.push(...str(name), ...str(type), ...int(value.length), ...value);
  bytes.push(...int(20000630), ...int(2));
  const channels = [];
  for (const name of ['B', 'G', 'R']) channels.push(...str(name), ...int(2), 0, 0, 0, 0, ...int(1), ...int(1));
  channels.push(0);
  attr('channels', 'chlist', channels); attr('compression', 'compression', [0]);
  const box = [...int(0), ...int(0), ...int(width - 1), ...int(height - 1)];
  attr('dataWindow', 'box2i', box); attr('displayWindow', 'box2i', box); attr('lineOrder', 'lineOrder', [0]);
  attr('pixelAspectRatio', 'float', float(1)); attr('screenWindowCenter', 'v2f', [...float(0), ...float(0)]); attr('screenWindowWidth', 'float', float(1));
  bytes.push(0);
  const rowSize = 8 + width * 12, start = bytes.length + height * 8;
  const result = new Uint8Array(start + height * rowSize); result.set(bytes); const view = new DataView(result.buffer);
  for (let y = 0; y < height; y++) {
    const row = start + y * rowSize; view.setBigUint64(bytes.length + y * 8, BigInt(row), true);
    view.setInt32(row, y, true); view.setInt32(row + 4, width * 12, true);
    for (let c = 0; c < 3; c++) for (let x = 0; x < width; x++) {
      const v = rgba[(y * width + x) * 4 + 2 - c];
      if (!Number.isFinite(v)) throw new Error('Cannot export non-finite radiance');
      view.setFloat32(row + 8 + (c * width + x) * 4, v, true);
    }
  }
  return result;
}
