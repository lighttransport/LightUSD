#!/usr/bin/env node
/**
 * HDRGen - Synthetic HDR/EXR Environment Map Generator
 *
 * Generates procedural environment maps for IBL testing and visualization
 * Supports: HDR (Radiance RGBE), EXR (OpenEXR), lat-long and cubemap projections
 *
 * Copyright 2024 - Present, Light Transport Entertainment Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

import * as fs from 'fs';
import * as path from 'path';
import * as zlib from 'zlib';

// ============================================================================
// Math Utilities
// ============================================================================

class Vec3 {
  constructor(x = 0, y = 0, z = 0) {
    this.x = x;
    this.y = y;
    this.z = z;
  }

  static add(a, b) {
    return new Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
  }

  static sub(a, b) {
    return new Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
  }

  static mul(v, s) {
    return new Vec3(v.x * s, v.y * s, v.z * s);
  }

  static dot(a, b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
  }

  static cross(a, b) {
    return new Vec3(
      a.y * b.z - a.z * b.y,
      a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x
    );
  }

  length() {
    return Math.sqrt(this.x * this.x + this.y * this.y + this.z * this.z);
  }

  normalize() {
    const len = this.length();
    if (len > 0) {
      return new Vec3(this.x / len, this.y / len, this.z / len);
    }
    return new Vec3(0, 0, 0);
  }

  static lerp(a, b, t) {
    return new Vec3(
      a.x + (b.x - a.x) * t,
      a.y + (b.y - a.y) * t,
      a.z + (b.z - a.z) * t
    );
  }
}

// ============================================================================
// HDR Image Buffer
// ============================================================================

class HDRImage {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    // Store as float32 RGB (linear color space)
    this.data = new Float32Array(width * height * 3);
  }

  setPixel(x, y, r, g, b) {
    const idx = (y * this.width + x) * 3;
    this.data[idx + 0] = r;
    this.data[idx + 1] = g;
    this.data[idx + 2] = b;
  }

  getPixel(x, y) {
    const idx = (y * this.width + x) * 3;
    return {
      r: this.data[idx + 0],
      g: this.data[idx + 1],
      b: this.data[idx + 2]
    };
  }

  // Convert lat-long (u,v) to direction vector
  static latLongToDir(u, v) {
    const phi = u * Math.PI * 2.0;   // 0 to 2π
    const theta = v * Math.PI;        // 0 to π
    const sinTheta = Math.sin(theta);
    return new Vec3(
      sinTheta * Math.cos(phi),
      Math.cos(theta),
      sinTheta * Math.sin(phi)
    );
  }

  // Convert direction vector to lat-long (u,v)
  static dirToLatLong(dir) {
    const theta = Math.acos(Math.max(-1, Math.min(1, dir.y)));
    const phi = Math.atan2(dir.z, dir.x);
    return {
      u: (phi + Math.PI) / (Math.PI * 2.0),
      v: theta / Math.PI
    };
  }
}

// ============================================================================
// Environment Importance Sampling
// ============================================================================

class ImportanceMap {
  /**
   * Build a two-level distribution for a lat-long environment map.
   * Texel weights include sin(theta), so sampling the resulting distribution
   * is proportional to luminance per unit solid angle.
   */
  static build(image) {
    const { width, height } = image;
    if (width < 1 || height < 1) {
      throw new Error('Importance maps require a non-empty image');
    }

    const rowCdf = new Float64Array(height + 1);
    const conditionalCdf = new Float64Array(height * (width + 1));
    const texelWeights = new Float64Array(width * height);
    const rowWeights = new Float64Array(height);
    let totalWeight = 0.0;

    for (let y = 0; y < height; y++) {
      const sinTheta = Math.sin(Math.PI * (y + 0.5) / height);
      const rowOffset = y * (width + 1);
      let rowWeight = 0.0;
      for (let x = 0; x < width; x++) {
        const pixel = image.getPixel(x, y);
        const luminance = Math.max(0.0,
          0.2126 * pixel.r + 0.7152 * pixel.g + 0.0722 * pixel.b);
        const weight = Number.isFinite(luminance) ? luminance * sinTheta : 0.0;
        texelWeights[y * width + x] = weight;
        rowWeight += weight;
        conditionalCdf[rowOffset + x + 1] = rowWeight;
      }

      // A black row still needs a valid conditional distribution. It will not
      // be selected unless the entire image is black.
      if (rowWeight > 0.0) {
        for (let x = 1; x <= width; x++) {
          conditionalCdf[rowOffset + x] /= rowWeight;
        }
      } else {
        for (let x = 1; x <= width; x++) {
          conditionalCdf[rowOffset + x] = x / width;
        }
      }
      rowWeights[y] = rowWeight;
      totalWeight += rowWeight;
      rowCdf[y + 1] = totalWeight;
    }

    if (totalWeight > 0.0) {
      for (let y = 1; y <= height; y++) rowCdf[y] /= totalWeight;
    } else {
      // Uniform radiance over a black image means uniform solid-angle sampling.
      totalWeight = 0.0;
      for (let y = 0; y < height; y++) {
        rowWeights[y] = Math.sin(Math.PI * (y + 0.5) / height) * width;
        totalWeight += rowWeights[y];
        rowCdf[y + 1] = totalWeight;
      }
      for (let y = 1; y <= height; y++) rowCdf[y] /= totalWeight;
      for (let i = 0; i < texelWeights.length; i++) {
        texelWeights[i] = rowWeights[Math.floor(i / width)] / width;
      }
    }

    return { width, height, totalWeight, rowCdf, conditionalCdf,
             rowWeights, texelWeights };
  }

  static _findInterval(cdf, offset, count, value) {
    const u = Math.max(0.0, Math.min(1.0 - Number.EPSILON, value));
    let low = 0;
    let high = count;
    while (low + 1 < high) {
      const mid = (low + high) >> 1;
      if (cdf[offset + mid] <= u) low = mid;
      else high = mid;
    }
    return low;
  }

  /** Sample at the texel center and return a PDF per steradian. */
  static sample(distribution, rowSample, columnSample) {
    const { width, height, rowCdf, conditionalCdf, texelWeights, totalWeight } = distribution;
    const y = ImportanceMap._findInterval(rowCdf, 0, height, rowSample);
    const x = ImportanceMap._findInterval(
      conditionalCdf, y * (width + 1), width, columnSample);
    const u = (x + 0.5) / width;
    const v = (y + 0.5) / height;
    const theta = Math.PI * v;
    const texelSolidAngle = (2.0 * Math.PI / width) *
      (Math.PI / height) * Math.sin(theta);
    const probability = texelWeights[y * width + x] / totalWeight;
    return {
      x, y, u, v,
      direction: HDRImage.latLongToDir(u, v),
      pdf: texelSolidAngle > 0.0 ? probability / texelSolidAngle : 0.0
    };
  }

  static writeJSON(distribution, filepath) {
    const payload = {
      version: 1,
      projection: 'latlong',
      width: distribution.width,
      height: distribution.height,
      totalWeight: distribution.totalWeight,
      rowCdf: Array.from(distribution.rowCdf),
      conditionalCdf: Array.from(distribution.conditionalCdf)
    };
    fs.writeFileSync(filepath, JSON.stringify(payload));
    console.log(`✓ Wrote importance map: ${filepath}`);
  }
}

// ============================================================================
// Image Transformation Utilities
// ============================================================================

class ImageTransform {
  /**
   * Rotate environment map around Y axis
   * @param {HDRImage} image - Source image
   * @param {number} angleDegrees - Rotation angle in degrees (positive = counterclockwise)
   * @returns {HDRImage} - Rotated image
   */
  static rotate(image, angleDegrees) {
    console.log(`Rotating environment map by ${angleDegrees}°...`);

    const rotated = new HDRImage(image.width, image.height);
    const angleRad = (angleDegrees * Math.PI) / 180.0;

    for (let y = 0; y < image.height; y++) {
      for (let x = 0; x < image.width; x++) {
        // Get current UV
        let u = x / image.width;
        const v = y / image.height;

        // Rotate U coordinate
        u = u + (angleRad / (Math.PI * 2.0));
        u = u - Math.floor(u); // Wrap to [0, 1]

        // Sample from source image with bilinear filtering
        const fx = u * (image.width - 1);
        const fy = v * (image.height - 1);

        const x0 = Math.floor(fx);
        const y0 = Math.floor(fy);
        const x1 = (x0 + 1) % image.width; // Wrap horizontally
        const y1 = Math.min(y0 + 1, image.height - 1);

        const tx = fx - x0;
        const ty = fy - y0;

        const c00 = image.getPixel(x0, y0);
        const c10 = image.getPixel(x1, y0);
        const c01 = image.getPixel(x0, y1);
        const c11 = image.getPixel(x1, y1);

        const r = (1 - tx) * (1 - ty) * c00.r + tx * (1 - ty) * c10.r +
                  (1 - tx) * ty * c01.r + tx * ty * c11.r;
        const g = (1 - tx) * (1 - ty) * c00.g + tx * (1 - ty) * c10.g +
                  (1 - tx) * ty * c01.g + tx * ty * c11.g;
        const b = (1 - tx) * (1 - ty) * c00.b + tx * (1 - ty) * c10.b +
                  (1 - tx) * ty * c01.b + tx * ty * c11.b;

        rotated.setPixel(x, y, r, g, b);
      }
    }

    return rotated;
  }

  /**
   * Scale intensity of entire image
   * @param {HDRImage} image - Image to scale (modified in place)
   * @param {number} scale - Intensity multiplier
   */
  static scaleIntensity(image, scale) {
    if (scale === 1.0) return;

    console.log(`Scaling intensity by ${scale}x...`);

    for (let i = 0; i < image.data.length; i++) {
      image.data[i] *= scale;
    }
  }
}

// ============================================================================
// Tone Mapping and LDR Conversion
// ============================================================================

class ToneMapper {
  /**
   * Apply tone mapping to HDR image for LDR display
   * @param {HDRImage} hdrImage - Source HDR image
   * @param {Object} options - Tone mapping options
   * @returns {Uint8ClampedArray} - 8-bit RGB data
   */
  static tonemapToLDR(hdrImage, options = {}) {
    const {
      exposure = 1.0,      // Exposure adjustment (EV)
      gamma = 2.2,         // Gamma correction for display
      method = 'reinhard'  // Tone mapping method: 'simple', 'reinhard', 'aces'
    } = options;

    console.log(`Tone mapping: method=${method}, exposure=${exposure}, gamma=${gamma}`);

    const { width, height, data } = hdrImage;
    const ldrData = new Uint8ClampedArray(width * height * 3);

    const exposureScale = Math.pow(2.0, exposure);
    const invGamma = 1.0 / gamma;

    for (let i = 0; i < data.length; i += 3) {
      let r = data[i + 0] * exposureScale;
      let g = data[i + 1] * exposureScale;
      let b = data[i + 2] * exposureScale;

      // Apply tone mapping operator
      switch (method) {
        case 'simple':
          // Simple exposure + clamp
          r = Math.min(r, 1.0);
          g = Math.min(g, 1.0);
          b = Math.min(b, 1.0);
          break;

        case 'reinhard':
          // Reinhard tone mapping: x / (1 + x)
          r = r / (1.0 + r);
          g = g / (1.0 + g);
          b = b / (1.0 + b);
          break;

        case 'aces':
          // ACES filmic tone mapping (approximation)
          r = ToneMapper.acesToneMap(r);
          g = ToneMapper.acesToneMap(g);
          b = ToneMapper.acesToneMap(b);
          break;

        default:
          r = Math.min(r, 1.0);
          g = Math.min(g, 1.0);
          b = Math.min(b, 1.0);
      }

      // Apply gamma correction
      r = Math.pow(Math.max(0, r), invGamma);
      g = Math.pow(Math.max(0, g), invGamma);
      b = Math.pow(Math.max(0, b), invGamma);

      // Convert to 8-bit
      ldrData[i + 0] = Math.round(Math.min(255, r * 255));
      ldrData[i + 1] = Math.round(Math.min(255, g * 255));
      ldrData[i + 2] = Math.round(Math.min(255, b * 255));
    }

    return ldrData;
  }

  /**
   * ACES filmic tone mapping curve
   */
  static acesToneMap(x) {
    const a = 2.51;
    const b = 0.03;
    const c = 2.43;
    const d = 0.59;
    const e = 0.14;
    return Math.min(1.0, Math.max(0.0, (x * (a * x + b)) / (x * (c * x + d) + e)));
  }
}

// ============================================================================
// LDR File Format Writers
// ============================================================================

class LDRWriter {
  /**
   * Write BMP format (24-bit RGB, uncompressed)
   */
  static writeBMP(ldrData, width, height, filepath) {
    // BMP requires rows to be padded to 4-byte boundary
    const rowSize = Math.floor((24 * width + 31) / 32) * 4;
    const pixelDataSize = rowSize * height;
    const fileSize = 54 + pixelDataSize; // 14-byte header + 40-byte DIB header + pixel data

    const buffer = Buffer.alloc(fileSize);

    // BMP Header (14 bytes)
    buffer.write('BM', 0); // Signature
    buffer.writeUInt32LE(fileSize, 2); // File size
    buffer.writeUInt32LE(0, 6); // Reserved
    buffer.writeUInt32LE(54, 10); // Pixel data offset

    // DIB Header (BITMAPINFOHEADER, 40 bytes)
    buffer.writeUInt32LE(40, 14); // DIB header size
    buffer.writeInt32LE(width, 18); // Width
    buffer.writeInt32LE(height, 22); // Height
    buffer.writeUInt16LE(1, 26); // Planes
    buffer.writeUInt16LE(24, 28); // Bits per pixel
    buffer.writeUInt32LE(0, 30); // Compression (0 = none)
    buffer.writeUInt32LE(pixelDataSize, 34); // Image size
    buffer.writeInt32LE(2835, 38); // X pixels per meter (72 DPI)
    buffer.writeInt32LE(2835, 42); // Y pixels per meter
    buffer.writeUInt32LE(0, 46); // Colors in palette
    buffer.writeUInt32LE(0, 50); // Important colors

    // Pixel data (bottom-up, BGR format)
    let offset = 54;
    for (let y = height - 1; y >= 0; y--) {
      for (let x = 0; x < width; x++) {
        const idx = (y * width + x) * 3;
        buffer[offset++] = ldrData[idx + 2]; // B
        buffer[offset++] = ldrData[idx + 1]; // G
        buffer[offset++] = ldrData[idx + 0]; // R
      }
      // Padding to 4-byte boundary
      while (offset % 4 !== 0) {
        buffer[offset++] = 0;
      }
    }

    fs.writeFileSync(filepath, buffer);
    console.log(`✓ Wrote BMP file: ${filepath}`);
  }

  /**
   * Write PNG format (8-bit RGB, uncompressed)
   * Simple implementation without compression
   */
  static writePNG(ldrData, width, height, filepath) {
    // For production, use a PNG library. This is a simplified implementation.
    // We'll write an uncompressed PNG using filter type 0 (None)

    const pngSignature = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);

    // IHDR chunk
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(width, 0);
    ihdr.writeUInt32BE(height, 4);
    ihdr.writeUInt8(8, 8); // Bit depth
    ihdr.writeUInt8(2, 9); // Color type (2 = RGB)
    ihdr.writeUInt8(0, 10); // Compression
    ihdr.writeUInt8(0, 11); // Filter
    ihdr.writeUInt8(0, 12); // Interlace

    // IDAT chunk (pixel data with filter bytes)
    // Each scanline: filter byte (0) + RGB data
    const scanlineSize = 1 + width * 3;
    const idatRaw = Buffer.alloc(scanlineSize * height);

    for (let y = 0; y < height; y++) {
      idatRaw[y * scanlineSize] = 0; // Filter type: None
      for (let x = 0; x < width; x++) {
        const srcIdx = (y * width + x) * 3;
        const dstIdx = y * scanlineSize + 1 + x * 3;
        idatRaw[dstIdx + 0] = ldrData[srcIdx + 0]; // R
        idatRaw[dstIdx + 1] = ldrData[srcIdx + 1]; // G
        idatRaw[dstIdx + 2] = ldrData[srcIdx + 2]; // B
      }
    }

    // Simple zlib compression would go here, but for now use uncompressed
    // For production, use zlib or a PNG library
    console.warn('PNG: Using simplified format (consider using sharp/pngjs for production)');

    // Build PNG file
    const chunks = [];
    chunks.push(pngSignature);
    chunks.push(LDRWriter.createPNGChunk('IHDR', ihdr));
    chunks.push(LDRWriter.createPNGChunk('IDAT', idatRaw));
    chunks.push(LDRWriter.createPNGChunk('IEND', Buffer.alloc(0)));

    const pngBuffer = Buffer.concat(chunks);
    fs.writeFileSync(filepath, pngBuffer);
    console.log(`✓ Wrote PNG file: ${filepath}`);
  }

  /**
   * Create PNG chunk with length, type, data, and CRC
   */
  static createPNGChunk(type, data) {
    const length = Buffer.alloc(4);
    length.writeUInt32BE(data.length, 0);

    const typeBuffer = Buffer.from(type, 'ascii');
    const crcData = Buffer.concat([typeBuffer, data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(LDRWriter.crc32(crcData), 0);

    return Buffer.concat([length, typeBuffer, data, crc]);
  }

  /**
   * CRC32 calculation for PNG
   */
  static crc32(buffer) {
    let crc = 0xFFFFFFFF;
    for (let i = 0; i < buffer.length; i++) {
      crc = crc ^ buffer[i];
      for (let j = 0; j < 8; j++) {
        crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
      }
    }
    return (crc ^ 0xFFFFFFFF) >>> 0; // Force unsigned 32-bit
  }

  /**
   * Write JPEG format
   * Note: Requires external library for production use
   */
  static writeJPEG(ldrData, width, height, filepath, quality = 90) {
    console.warn('JPEG writing requires external library (e.g., jpeg-js)');
    console.warn('Converting to BMP instead');
    const bmpPath = filepath.replace(/\.jpe?g$/i, '.bmp');
    LDRWriter.writeBMP(ldrData, width, height, bmpPath);
  }
}

// ============================================================================
// HDR File Format Writers
// ============================================================================

class HDRWriter {
  /**
   * Write Radiance RGBE (.hdr) format
   * https://en.wikipedia.org/wiki/RGBE_image_format
   */
  static writeRGBE(image, filepath) {
    const { width, height, data } = image;

    // RGBE encoding function
    function encodeRGBE(r, g, b) {
      const maxComp = Math.max(r, g, b);
      if (maxComp < 1e-32) {
        return Buffer.from([0, 0, 0, 0]);
      }

      const exponent = Math.floor(Math.log2(maxComp)) + 128;
      const scale = Math.pow(2, exponent - 128);

      const re = Math.floor((r / scale) * 255.0 + 0.5);
      const ge = Math.floor((g / scale) * 255.0 + 0.5);
      const be = Math.floor((b / scale) * 255.0 + 0.5);

      return Buffer.from([
        Math.min(255, re),
        Math.min(255, ge),
        Math.min(255, be),
        exponent
      ]);
    }

    // Build HDR header
    const header = [
      '#?RADIANCE',
      'FORMAT=32-bit_rle_rgbe',
      `EXPOSURE=1.0`,
      '',
      `-Y ${height} +X ${width}`,
      ''
    ].join('\n');

    const headerBuf = Buffer.from(header, 'ascii');

    // Encode pixel data
    const pixelBuf = Buffer.alloc(width * height * 4);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const idx = (y * width + x) * 3;
        const r = data[idx + 0];
        const g = data[idx + 1];
        const b = data[idx + 2];
        const rgbe = encodeRGBE(r, g, b);
        rgbe.copy(pixelBuf, (y * width + x) * 4);
      }
    }

    // Write file
    const fullBuf = Buffer.concat([headerBuf, pixelBuf]);
    fs.writeFileSync(filepath, fullBuf);
    console.log(`✓ Wrote HDR file: ${filepath}`);
  }

  /** Write an OpenEXR v2 image with float scanlines. */
  static writeEXR(image, filepath, compression = 'zips') {
    const { width, height, data } = image;
    if (width < 1 || height < 1) throw new Error('EXR dimensions must be positive');
    if (!['none', 'zips'].includes(compression)) {
      throw new Error(`Unsupported EXR compression: ${compression}`);
    }

    const cstring = value => Buffer.from(`${value}\0`, 'ascii');
    const attribute = (name, type, value) => {
      const size = Buffer.alloc(4);
      size.writeUInt32LE(value.length, 0);
      return Buffer.concat([cstring(name), cstring(type), size, value]);
    };
    const float32 = value => {
      const result = Buffer.alloc(4);
      result.writeFloatLE(value, 0);
      return result;
    };
    const box2i = Buffer.alloc(16);
    box2i.writeInt32LE(0, 0);
    box2i.writeInt32LE(0, 4);
    box2i.writeInt32LE(width - 1, 8);
    box2i.writeInt32LE(height - 1, 12);

    const channels = [];
    for (const name of ['B', 'G', 'R']) {
      // pixel type (2=float), pLinear/reserved, xSampling, ySampling.
      const descriptor = Buffer.alloc(16);
      descriptor.writeInt32LE(2, 0);
      descriptor.writeUInt8(0, 4);
      descriptor.writeInt32LE(1, 8);
      descriptor.writeInt32LE(1, 12);
      channels.push(cstring(name), descriptor);
    }
    channels.push(Buffer.from([0]));

    const header = Buffer.concat([
      attribute('channels', 'chlist', Buffer.concat(channels)),
      attribute('compression', 'compression', Buffer.from([compression === 'zips' ? 2 : 0])),
      attribute('dataWindow', 'box2i', box2i),
      attribute('displayWindow', 'box2i', box2i),
      attribute('lineOrder', 'lineOrder', Buffer.from([0])),
      attribute('pixelAspectRatio', 'float', float32(1.0)),
      attribute('screenWindowCenter', 'v2f', Buffer.alloc(8)),
      attribute('screenWindowWidth', 'float', float32(1.0)),
      Buffer.from([0])
    ]);
    const prefix = Buffer.alloc(8);
    prefix.writeUInt32LE(20000630, 0);
    prefix.writeUInt32LE(2, 4);

    const bytesPerScanline = width * 3 * 4;
    const blocks = [];
    for (let y = 0; y < height; y++) {
      const scanline = Buffer.alloc(bytesPerScanline);
      let dst = 0;
      // Channel samples are contiguous and follow chlist order.
      for (const channel of [2, 1, 0]) {
        for (let x = 0; x < width; x++) {
          const value = data[(y * width + x) * 3 + channel];
          scanline.writeFloatLE(Number.isFinite(value) ? value : 0.0, dst);
          dst += 4;
        }
      }
      const packed = compression === 'zips' ? HDRWriter._zipEXRBytes(scanline) : scanline;
      const block = Buffer.alloc(8 + packed.length);
      block.writeInt32LE(y, 0);
      block.writeUInt32LE(packed.length, 4);
      packed.copy(block, 8);
      blocks.push(block);
    }

    const offsetTable = Buffer.alloc(height * 8);
    let blockOffset = BigInt(prefix.length + header.length + offsetTable.length);
    for (let y = 0; y < height; y++) {
      offsetTable.writeBigUInt64LE(blockOffset, y * 8);
      blockOffset += BigInt(blocks[y].length);
    }
    fs.writeFileSync(filepath, Buffer.concat([prefix, header, offsetTable, ...blocks]));
    console.log(`✓ Wrote EXR file (${compression}): ${filepath}`);
  }

  static _zipEXRBytes(raw) {
    const reordered = Buffer.alloc(raw.length);
    let even = 0;
    let odd = Math.floor((raw.length + 1) / 2);
    for (let i = 0; i < raw.length; i += 2) reordered[even++] = raw[i];
    for (let i = 1; i < raw.length; i += 2) reordered[odd++] = raw[i];

    let previous = reordered[0];
    for (let i = 1; i < reordered.length; i++) {
      const current = reordered[i];
      reordered[i] = (current - previous + 128) & 0xff;
      previous = current;
    }
    const compressed = zlib.deflateSync(reordered);
    // OpenEXR permits an uncompressed block when compression expands the data.
    return compressed.length < raw.length ? compressed : raw;
  }
}

// ============================================================================
// Environment Map Presets
// ============================================================================

class EnvMapPresets {
  /**
   * White Furnace - Uniform white environment for energy conservation testing
   * Perfect for validating that BRDF integrates to 1.0
   */
  static whiteFurnace(image, intensity = 1.0) {
    console.log(`Generating White Furnace (${image.width}x${image.height})...`);

    for (let y = 0; y < image.height; y++) {
      for (let x = 0; x < image.width; x++) {
        image.setPixel(x, y, intensity, intensity, intensity);
      }
    }
  }

  /**
   * Sun & Sky - Procedural Hosek-Wilkie sky model approximation
   * Simplified version with sun disk and gradient sky
   */
  static sunSky(image, options = {}) {
    const {
      sunElevation = 45,      // Sun elevation in degrees (0 = horizon, 90 = zenith)
      sunAzimuth = 135,       // Sun azimuth in degrees (0 = north, 90 = east)
      sunIntensity = 100.0,   // Sun disk intensity
      sunRadius = 0.02,       // Sun angular radius (radians)
      skyIntensity = 0.5,     // Base sky intensity
      horizonColor = new Vec3(0.8, 0.9, 1.0),  // Horizon tint
      zenithColor = new Vec3(0.3, 0.5, 0.9),   // Zenith color
      sunColor = new Vec3(1.0, 0.95, 0.8),     // Sun disk tint
    } = options;

    console.log(`Generating Sun & Sky (${image.width}x${image.height})...`);
    console.log(`  Sun: elevation=${sunElevation}°, azimuth=${sunAzimuth}°, intensity=${sunIntensity}`);

    // Convert sun angles to direction
    const elevRad = sunElevation * Math.PI / 180;
    const azimRad = sunAzimuth * Math.PI / 180;
    const sunDir = new Vec3(
      Math.cos(elevRad) * Math.cos(azimRad),
      Math.sin(elevRad),
      Math.cos(elevRad) * Math.sin(azimRad)
    ).normalize();

    for (let y = 0; y < image.height; y++) {
      for (let x = 0; x < image.width; x++) {
        const u = x / image.width;
        const v = y / image.height;

        const dir = HDRImage.latLongToDir(u, v);

        // Sky gradient based on elevation
        const elevation = Math.asin(Math.max(-1, Math.min(1, dir.y)));
        const elevNorm = (elevation + Math.PI / 2) / Math.PI; // 0 at bottom, 1 at top

        // Interpolate between horizon and zenith
        const skyColor = Vec3.lerp(horizonColor, zenithColor, elevNorm);
        let r = skyColor.x * skyIntensity;
        let g = skyColor.y * skyIntensity;
        let b = skyColor.z * skyIntensity;

        // Add sun disk
        const angleToCosun = Vec3.dot(dir, sunDir);
        const angleToSun = Math.acos(Math.max(-1, Math.min(1, angleToCosun)));

        if (angleToSun < sunRadius) {
          // Inside sun disk
          const falloff = 1.0 - (angleToSun / sunRadius);
          const sunCol = Vec3.mul(sunColor, sunIntensity);
          r += sunCol.x * falloff;
          g += sunCol.y * falloff;
          b += sunCol.z * falloff;
        } else if (angleToSun < sunRadius * 3) {
          // Sun glow
          const falloff = 1.0 - ((angleToSun - sunRadius) / (sunRadius * 2));
          const glowIntensity = sunIntensity * 0.1 * falloff * falloff;
          r += glowIntensity;
          g += glowIntensity * 0.9;
          b += glowIntensity * 0.7;
        }

        image.setPixel(x, y, r, g, b);
      }
    }
  }

  /**
   * CIE-style overcast sky. Luminance rises smoothly toward the zenith while
   * the lower hemisphere receives a dim, neutral ground bounce.
   */
  static overcast(image, options = {}) {
    const {
      skyIntensity = 1.0,
      groundIntensity = 0.08,
      horizonColor = new Vec3(0.72, 0.78, 0.84),
      zenithColor = new Vec3(0.45, 0.56, 0.68),
      groundColor = new Vec3(0.35, 0.36, 0.37)
    } = options;

    console.log(`Generating Overcast Sky (${image.width}x${image.height})...`);
    for (let y = 0; y < image.height; y++) {
      for (let x = 0; x < image.width; x++) {
        const dir = HDRImage.latLongToDir(
          (x + 0.5) / image.width, (y + 0.5) / image.height);
        if (dir.y >= 0.0) {
          const elevationFactor = (1.0 + 2.0 * dir.y) / 3.0;
          const tint = Vec3.lerp(horizonColor, zenithColor, dir.y);
          image.setPixel(x, y,
            tint.x * skyIntensity * elevationFactor,
            tint.y * skyIntensity * elevationFactor,
            tint.z * skyIntensity * elevationFactor);
        } else {
          const fade = 0.65 + 0.35 * (1.0 + dir.y);
          image.setPixel(x, y,
            groundColor.x * groundIntensity * fade,
            groundColor.y * groundIntensity * fade,
            groundColor.z * groundIntensity * fade);
        }
      }
    }
  }

  /**
   * Studio Lighting - 3-point lighting setup
   * Key light (main), fill light (shadows), rim/back light
   */
  static studioLighting(image, options = {}) {
    const {
      keyIntensity = 50.0,
      fillIntensity = 10.0,
      rimIntensity = 20.0,
      ambientIntensity = 0.5,
      keyColor = new Vec3(1.0, 0.98, 0.95),     // Warm key
      fillColor = new Vec3(0.8, 0.85, 1.0),     // Cool fill
      rimColor = new Vec3(1.0, 1.0, 1.0),       // White rim
      ambientColor = new Vec3(0.5, 0.5, 0.5),   // Neutral ambient
    } = options;

    console.log(`Generating Studio Lighting (${image.width}x${image.height})...`);
    console.log(`  Key=${keyIntensity}, Fill=${fillIntensity}, Rim=${rimIntensity}`);

    // Light positions (as directions)
    const keyLight = new Vec3(0.7, 0.5, 0.5).normalize();      // Front-right, elevated
    const fillLight = new Vec3(-0.5, 0.3, 0.3).normalize();    // Front-left, lower
    const rimLight = new Vec3(0, 0.4, -0.9).normalize();       // Back, elevated

    // Light spreads (angular size in radians)
    const keySpread = 0.3;
    const fillSpread = 0.5;
    const rimSpread = 0.2;

    for (let y = 0; y < image.height; y++) {
      for (let x = 0; x < image.width; x++) {
        const u = x / image.width;
        const v = y / image.height;

        const dir = HDRImage.latLongToDir(u, v);

        // Start with ambient
        let r = ambientColor.x * ambientIntensity;
        let g = ambientColor.y * ambientIntensity;
        let b = ambientColor.z * ambientIntensity;

        // Add key light
        const keyDot = Math.max(0, Vec3.dot(dir, keyLight));
        const keyAngle = Math.acos(Math.max(0, Math.min(1, keyDot)));
        if (keyAngle < keySpread) {
          const falloff = Math.pow(1.0 - (keyAngle / keySpread), 2);
          r += keyColor.x * keyIntensity * falloff;
          g += keyColor.y * keyIntensity * falloff;
          b += keyColor.z * keyIntensity * falloff;
        }

        // Add fill light
        const fillDot = Math.max(0, Vec3.dot(dir, fillLight));
        const fillAngle = Math.acos(Math.max(0, Math.min(1, fillDot)));
        if (fillAngle < fillSpread) {
          const falloff = Math.pow(1.0 - (fillAngle / fillSpread), 2);
          r += fillColor.x * fillIntensity * falloff;
          g += fillColor.y * fillIntensity * falloff;
          b += fillColor.z * fillIntensity * falloff;
        }

        // Add rim light
        const rimDot = Math.max(0, Vec3.dot(dir, rimLight));
        const rimAngle = Math.acos(Math.max(0, Math.min(1, rimDot)));
        if (rimAngle < rimSpread) {
          const falloff = Math.pow(1.0 - (rimAngle / rimSpread), 3);
          r += rimColor.x * rimIntensity * falloff;
          g += rimColor.y * rimIntensity * falloff;
          b += rimColor.z * rimIntensity * falloff;
        }

        image.setPixel(x, y, r, g, b);
      }
    }
  }
}

// ============================================================================
// Cubemap Generator
// ============================================================================

class CubemapGenerator {
  /**
   * Generate 6 cubemap faces from an equirectangular environment map
   * Face order: +X, -X, +Y, -Y, +Z, -Z (standard OpenGL order)
   */
  static fromLatLong(latLongImage, faceSize = 512) {
    console.log(`Converting lat-long to cubemap (face size: ${faceSize}x${faceSize})...`);

    const faces = [];
    const faceNames = ['+X', '-X', '+Y', '-Y', '+Z', '-Z'];

    // Cubemap face directions
    const faceData = [
      // +X (right)
      { right: new Vec3(0, 0, -1), up: new Vec3(0, 1, 0), forward: new Vec3(1, 0, 0) },
      // -X (left)
      { right: new Vec3(0, 0, 1), up: new Vec3(0, 1, 0), forward: new Vec3(-1, 0, 0) },
      // +Y (top)
      { right: new Vec3(1, 0, 0), up: new Vec3(0, 0, -1), forward: new Vec3(0, 1, 0) },
      // -Y (bottom)
      { right: new Vec3(1, 0, 0), up: new Vec3(0, 0, 1), forward: new Vec3(0, -1, 0) },
      // +Z (front)
      { right: new Vec3(1, 0, 0), up: new Vec3(0, 1, 0), forward: new Vec3(0, 0, 1) },
      // -Z (back)
      { right: new Vec3(-1, 0, 0), up: new Vec3(0, 1, 0), forward: new Vec3(0, 0, -1) },
    ];

    for (let faceIdx = 0; faceIdx < 6; faceIdx++) {
      const face = new HDRImage(faceSize, faceSize);
      const { right, up, forward } = faceData[faceIdx];

      for (let y = 0; y < faceSize; y++) {
        for (let x = 0; x < faceSize; x++) {
          // Map to [-1, 1] range
          const u = (x / (faceSize - 1)) * 2.0 - 1.0;
          const v = (y / (faceSize - 1)) * 2.0 - 1.0;

          // Get direction for this texel
          const dir = Vec3.add(
            Vec3.add(Vec3.mul(right, u), Vec3.mul(up, -v)),
            forward
          ).normalize();

          // Convert to lat-long coords and sample
          const { u: latU, v: latV } = HDRImage.dirToLatLong(dir);
          const color = CubemapGenerator.sampleBilinear(latLongImage, latU, latV);

          face.setPixel(x, y, color.r, color.g, color.b);
        }
      }

      faces.push({ image: face, name: faceNames[faceIdx] });
    }

    return faces;
  }

  /**
   * Bilinear sampling from lat-long image
   */
  static sampleBilinear(image, u, v) {
    // Wrap u, clamp v
    u = u - Math.floor(u);
    v = Math.max(0, Math.min(1, v));

    const fx = u * (image.width - 1);
    const fy = v * (image.height - 1);

    const x0 = Math.floor(fx);
    const y0 = Math.floor(fy);
    const x1 = Math.min(x0 + 1, image.width - 1);
    const y1 = Math.min(y0 + 1, image.height - 1);

    const tx = fx - x0;
    const ty = fy - y0;

    const c00 = image.getPixel(x0, y0);
    const c10 = image.getPixel(x1, y0);
    const c01 = image.getPixel(x0, y1);
    const c11 = image.getPixel(x1, y1);

    const r = (1 - tx) * (1 - ty) * c00.r + tx * (1 - ty) * c10.r +
              (1 - tx) * ty * c01.r + tx * ty * c11.r;
    const g = (1 - tx) * (1 - ty) * c00.g + tx * (1 - ty) * c10.g +
              (1 - tx) * ty * c01.g + tx * ty * c11.g;
    const b = (1 - tx) * (1 - ty) * c00.b + tx * (1 - ty) * c10.b +
              (1 - tx) * ty * c01.b + tx * ty * c11.b;

    return { r, g, b };
  }
}

// ============================================================================
// IBL Prefiltering
// ============================================================================

class EnvironmentPrefilter {
  static _radicalInverse(bits) {
    bits = ((bits << 16) | (bits >>> 16)) >>> 0;
    bits = (((bits & 0x55555555) << 1) | ((bits & 0xaaaaaaaa) >>> 1)) >>> 0;
    bits = (((bits & 0x33333333) << 2) | ((bits & 0xcccccccc) >>> 2)) >>> 0;
    bits = (((bits & 0x0f0f0f0f) << 4) | ((bits & 0xf0f0f0f0) >>> 4)) >>> 0;
    bits = (((bits & 0x00ff00ff) << 8) | ((bits & 0xff00ff00) >>> 8)) >>> 0;
    return bits * 2.3283064365386963e-10;
  }

  static _basis(normal) {
    const up = Math.abs(normal.y) < 0.999 ? new Vec3(0, 1, 0) : new Vec3(1, 0, 0);
    const tangent = Vec3.cross(up, normal).normalize();
    return { tangent, bitangent: Vec3.cross(normal, tangent) };
  }

  static _toWorld(local, normal, basis) {
    return Vec3.add(Vec3.add(
      Vec3.mul(basis.tangent, local.x),
      Vec3.mul(basis.bitangent, local.z)),
      Vec3.mul(normal, local.y)).normalize();
  }

  static _sampleDirection(image, direction) {
    const phi = Math.atan2(direction.z, direction.x);
    const u = phi / (2.0 * Math.PI) - Math.floor(phi / (2.0 * Math.PI));
    const v = Math.acos(Math.max(-1, Math.min(1, direction.y))) / Math.PI;
    return CubemapGenerator.sampleBilinear(image, u, v);
  }

  static diffuse(image, width = 32, height = 16, sampleCount = 64) {
    const output = new HDRImage(width, height);
    const count = Math.max(1, Math.floor(sampleCount));
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const normal = HDRImage.latLongToDir((x + 0.5) / width, (y + 0.5) / height);
        const basis = EnvironmentPrefilter._basis(normal);
        let r = 0, g = 0, b = 0;
        for (let i = 0; i < count; i++) {
          const u = (i + 0.5) / count;
          const v = EnvironmentPrefilter._radicalInverse(i);
          const radius = Math.sqrt(u);
          const angle = 2.0 * Math.PI * v;
          const local = new Vec3(
            radius * Math.cos(angle), Math.sqrt(1.0 - u), radius * Math.sin(angle));
          const color = EnvironmentPrefilter._sampleDirection(
            image, EnvironmentPrefilter._toWorld(local, normal, basis));
          r += color.r;
          g += color.g;
          b += color.b;
        }
        const scale = Math.PI / count;
        output.setPixel(x, y, r * scale, g * scale, b * scale);
      }
    }
    return output;
  }

  static specularLevel(image, roughness, width, height, sampleCount = 64) {
    const output = new HDRImage(width, height);
    const count = Math.max(1, Math.floor(sampleCount));
    const alpha = Math.max(0.001, roughness * roughness);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const normal = HDRImage.latLongToDir((x + 0.5) / width, (y + 0.5) / height);
        const basis = EnvironmentPrefilter._basis(normal);
        let r = 0, g = 0, b = 0, weight = 0;
        for (let i = 0; i < count; i++) {
          const u = (i + 0.5) / count;
          const v = EnvironmentPrefilter._radicalInverse(i);
          const phi = 2.0 * Math.PI * v;
          const cosTheta = Math.sqrt((1.0 - u) /
            (1.0 + (alpha * alpha - 1.0) * u));
          const sinTheta = Math.sqrt(Math.max(0.0, 1.0 - cosTheta * cosTheta));
          const halfVector = EnvironmentPrefilter._toWorld(
            new Vec3(sinTheta * Math.cos(phi), cosTheta, sinTheta * Math.sin(phi)),
            normal, basis);
          const viewDotHalf = Math.max(0.0, Vec3.dot(normal, halfVector));
          const light = Vec3.sub(Vec3.mul(halfVector, 2.0 * viewDotHalf), normal).normalize();
          const nDotL = Math.max(0.0, Vec3.dot(normal, light));
          if (nDotL <= 0.0) continue;
          const color = EnvironmentPrefilter._sampleDirection(image, light);
          r += color.r * nDotL;
          g += color.g * nDotL;
          b += color.b * nDotL;
          weight += nDotL;
        }
        const inverseWeight = weight > 0.0 ? 1.0 / weight : 0.0;
        output.setPixel(x, y, r * inverseWeight, g * inverseWeight, b * inverseWeight);
      }
    }
    return output;
  }

  static generate(image, options = {}) {
    const {
      directory,
      width = 64,
      height = Math.max(1, Math.floor(width / 2)),
      levels = 6,
      samples = 64,
      exrCompression = 'zips'
    } = options;
    if (!directory) throw new Error('Prefilter output directory is required');
    fs.mkdirSync(directory, { recursive: true });

    const diffuse = EnvironmentPrefilter.diffuse(
      image, Math.max(1, Math.floor(width / 2)), Math.max(1, Math.floor(height / 2)), samples);
    HDRWriter.writeEXR(diffuse, path.join(directory, 'diffuse.exr'), exrCompression);

    const specular = [];
    const levelCount = Math.max(1, Math.floor(levels));
    for (let level = 0; level < levelCount; level++) {
      const roughness = levelCount === 1 ? 0.0 : level / (levelCount - 1);
      const levelWidth = Math.max(1, Math.floor(width / Math.pow(2, level)));
      const levelHeight = Math.max(1, Math.floor(height / Math.pow(2, level)));
      const filtered = EnvironmentPrefilter.specularLevel(
        image, roughness, levelWidth, levelHeight, samples);
      const filename = `specular-${String(level).padStart(2, '0')}.exr`;
      HDRWriter.writeEXR(filtered, path.join(directory, filename), exrCompression);
      specular.push({ image: filtered, roughness, filename });
    }
    const manifest = {
      version: 1,
      projection: 'latlong',
      diffuse: 'diffuse.exr',
      specular: specular.map(level => ({
        file: level.filename,
        roughness: level.roughness,
        width: level.image.width,
        height: level.image.height
      }))
    };
    fs.writeFileSync(path.join(directory, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
    return { diffuse, specular, manifest };
  }
}

// ============================================================================
// Public API
// ============================================================================

export class HDRGenerator {
  /**
   * Generate environment map with specified preset
   *
   * @param {Object} options - Generation options
   * @param {string} options.preset - Preset name: 'white-furnace', 'sun-sky',
   *   'sunset', 'overcast', or 'studio'
   * @param {number} options.width - Width in pixels (default: 2048 for latlong, 512 for cubemap)
   * @param {number} options.height - Height in pixels (default: 1024 for latlong)
   * @param {string} options.projection - 'latlong' or 'cubemap'
   * @param {string} options.format - 'hdr', 'exr', 'png', 'bmp', 'jpg'/'jpeg'
   * @param {string} options.output - Output file path
   * @param {Object} options.presetOptions - Preset-specific options
   * @param {number} options.rotation - Rotation angle in degrees (default: 0)
   * @param {number} options.intensityScale - Intensity multiplier (default: 1.0)
   * @param {Object} options.tonemapOptions - Tone mapping options for LDR output
   * @param {string} options.importanceMap - Optional JSON importance-map path
   * @param {string} options.exrCompression - 'zips' or 'none'
   * @param {Object} options.prefilter - Optional IBL prefilter options
   */
  static generate(options) {
    const {
      preset = 'white-furnace',
      width = 2048,
      height = 1024,
      projection = 'latlong',
      format = 'hdr',
      output = null,
      presetOptions = {},
      rotation = 0,
      intensityScale = 1.0,
      tonemapOptions = {},
      importanceMap = null,
      exrCompression = 'zips',
      prefilter = null
    } = options;

    console.log('\n=== HDR Environment Map Generator ===');
    console.log(`Preset: ${preset}`);
    console.log(`Resolution: ${width}x${height}`);
    console.log(`Projection: ${projection}`);
    console.log(`Format: ${format.toUpperCase()}`);
    if (rotation !== 0) console.log(`Rotation: ${rotation}°`);
    if (intensityScale !== 1.0) console.log(`Intensity Scale: ${intensityScale}x`);

    // Generate lat-long image first
    let latLongImage = new HDRImage(width, height);

    // Apply preset
    switch (preset) {
      case 'white-furnace':
        EnvMapPresets.whiteFurnace(latLongImage, presetOptions.intensity || 1.0);
        break;
      case 'sun-sky':
        EnvMapPresets.sunSky(latLongImage, presetOptions);
        break;
      case 'sunset':
        EnvMapPresets.sunSky(latLongImage, {
          sunElevation: 5,
          sunAzimuth: 270,
          sunIntensity: 150.0,
          skyIntensity: 0.65,
          horizonColor: new Vec3(1.0, 0.38, 0.12),
          zenithColor: new Vec3(0.12, 0.16, 0.38),
          sunColor: new Vec3(1.0, 0.42, 0.12),
          ...presetOptions
        });
        break;
      case 'overcast':
        EnvMapPresets.overcast(latLongImage, presetOptions);
        break;
      case 'studio':
        EnvMapPresets.studioLighting(latLongImage, presetOptions);
        break;
      default:
        throw new Error(`Unknown preset: ${preset}`);
    }

    // Apply transformations
    if (rotation !== 0) {
      latLongImage = ImageTransform.rotate(latLongImage, rotation);
    }

    if (intensityScale !== 1.0) {
      ImageTransform.scaleIntensity(latLongImage, intensityScale);
    }

    const importanceDistribution = importanceMap ? ImportanceMap.build(latLongImage) : null;
    if (importanceMap) ImportanceMap.writeJSON(importanceDistribution, importanceMap);
    const prefiltered = prefilter ? EnvironmentPrefilter.generate(latLongImage, {
      ...prefilter, exrCompression
    }) : null;

    // Determine if output is LDR or HDR
    const isLDR = ['png', 'bmp', 'jpg', 'jpeg'].includes(format.toLowerCase());

    // Generate output
    if (projection === 'latlong') {
      // Direct lat-long output
      if (output) {
        const filepath = output.endsWith(`.${format}`) ? output : `${output}.${format}`;
        HDRGenerator._writeImage(latLongImage, format, filepath, isLDR,
          tonemapOptions, exrCompression);
      }
      return { latLongImage, importanceMap: importanceDistribution, prefiltered };
    } else if (projection === 'cubemap') {
      // Convert to cubemap
      const faceSize = Math.min(width, height); // Use smaller dimension for cube face
      const faces = CubemapGenerator.fromLatLong(latLongImage, faceSize);

      if (output) {
        const dir = path.dirname(output);
        const base = path.basename(output, path.extname(output));

        for (const face of faces) {
          const facePath = path.join(dir, `${base}_${face.name}.${format}`);
          HDRGenerator._writeImage(face.image, format, facePath, isLDR,
            tonemapOptions, exrCompression);
        }
      }
      return { faces, importanceMap: importanceDistribution, prefiltered };
    }
  }

  /**
   * Internal helper to write image in appropriate format
   */
  static _writeImage(image, format, filepath, isLDR, tonemapOptions,
                     exrCompression = 'zips') {
    if (isLDR) {
      // Convert HDR to LDR via tone mapping
      const ldrData = ToneMapper.tonemapToLDR(image, tonemapOptions);
      const fmt = format.toLowerCase();

      switch (fmt) {
        case 'png':
          LDRWriter.writePNG(ldrData, image.width, image.height, filepath);
          break;
        case 'bmp':
          LDRWriter.writeBMP(ldrData, image.width, image.height, filepath);
          break;
        case 'jpg':
        case 'jpeg':
          LDRWriter.writeJPEG(ldrData, image.width, image.height, filepath);
          break;
        default:
          throw new Error(`Unknown LDR format: ${format}`);
      }
    } else {
      // HDR output
      if (format === 'hdr') {
        HDRWriter.writeRGBE(image, filepath);
      } else if (format === 'exr') {
        HDRWriter.writeEXR(image, filepath, exrCompression);
      } else {
        throw new Error(`Unknown HDR format: ${format}`);
      }
    }
  }
}

export {
  EnvMapPresets,
  HDRImage,
  CubemapGenerator,
  HDRWriter,
  LDRWriter,
  ToneMapper,
  ImageTransform,
  ImportanceMap,
  EnvironmentPrefilter,
  Vec3
};
