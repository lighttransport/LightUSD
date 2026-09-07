// SPDX-License-Identifier: Apache-2.0
// Decoded image resources: RGBA float data, row zero at MaterialX v=0.
// Preview working space is linear Rec.709; ACEScg transport is a later milestone.
export function packImages(images, { maxBytes = 64 * 1024 * 1024 } = {}) {
  const chunks = [], descriptors = []; let texels = 0;
  for (const image of images) {
    const { width, height, data, colorspace = 'lin_rec709' } = image;
    if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width > 16384 || height > 16384) throw new Error('Invalid image dimensions');
    if (!data || data.length !== width * height * 4) throw new Error('Expected RGBA image data');
    if (!['lin_rec709', 'srgb_texture', 'raw', 'acescg', 'ACEScg'].includes(colorspace)) throw new Error(`Unsupported image colorspace: ${colorspace}`);
    let w = width, h = height, count = 0;
    do { count += w * h; if (w === 1 && h === 1) break; w = Math.max(1, Math.floor(w / 2)); h = Math.max(1, Math.floor(h / 2)); } while (true);
    if ((texels + count) * 16 > maxBytes) throw new Error('Image mip chain exceeds texture budget');
    let pixels = Float32Array.from(data);
    if (!pixels.every(Number.isFinite)) throw new Error('Image contains non-finite float32 values');
    if (colorspace === 'srgb_texture') for (let i = 0; i < pixels.length; i++) if (i % 4 !== 3) {
      const c = pixels[i]; pixels[i] = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
    }
    // MaterialX 1.39.5 cmlib NG_acescg_to_lin_rec709_color3 matrix.
    // Keep negative out-of-gamut values; alpha is not color transformed.
    if (colorspace.toLowerCase() === 'acescg') for (let i = 0; i < pixels.length; i += 4) {
      const r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
      pixels[i] = 1.705050992658 * r - .621792120657 * g - .083258872001 * b;
      pixels[i + 1] = -.130256417507 * r + 1.140804736575 * g - .010548319068 * b;
      pixels[i + 2] = -.024003356805 * r - .128968976065 * g + 1.15297233287 * b;
    }
    if (!pixels.every(Number.isFinite)) throw new Error('Image colorspace conversion overflows float32');
    const descriptor = { offset: texels, width, height, levels: 0 }; descriptors.push(descriptor);
    w = width; h = height;
    while (true) {
      chunks.push(pixels); texels += w * h; descriptor.levels++;
      if (w === 1 && h === 1) break;
      const nw = Math.max(1, Math.floor(w / 2)), nh = Math.max(1, Math.floor(h / 2)), next = new Float32Array(nw * nh * 4);
      // Area box filtering preserves all texels of odd-sized images.
      for (let y = 0; y < nh; y++) for (let x = 0; x < nw; x++) {
        const x0 = x * w / nw, x1 = (x + 1) * w / nw, y0 = y * h / nh, y1 = (y + 1) * h / nh;
        for (let iy = Math.floor(y0); iy < Math.ceil(y1); iy++) for (let ix = Math.floor(x0); ix < Math.ceil(x1); ix++) {
          const weight = (Math.min(ix + 1, x1) - Math.max(ix, x0)) * (Math.min(iy + 1, y1) - Math.max(iy, y0)) / ((x1 - x0) * (y1 - y0));
          for (let k = 0; k < 4; k++) next[(y * nw + x) * 4 + k] += pixels[(iy * w + ix) * 4 + k] * weight;
        }
      }
      pixels = next; w = nw; h = nh;
    }
  }
  const data = new Float32Array(Math.max(4, texels * 4)); let offset = 0;
  for (const chunk of chunks) { data.set(chunk, offset); offset += chunk.length; }
  return { data, descriptors };
}

export const imageWGSL = /* wgsl */`
@group(0) @binding(4) var<storage,read> imagePixels: array<vec4f>;
fn imageAddress(i: i32, size: i32, mode: u32) -> i32 {
  if (mode == 1u) { return clamp(i,0,size-1); }
  if (mode == 2u) { return ((i % size)+size)%size; }
  if (mode == 3u) { let p=((i%(2*size))+2*size)%(2*size); return min(p,2*size-1-p); }
  return i;
}
fn imageTexel(offset: u32, size: vec2u, p: vec2i, address: vec2u, fallback: vec4f) -> vec4f {
  let q=vec2i(imageAddress(p.x,i32(size.x),address.x),imageAddress(p.y,i32(size.y),address.y));
  if (any(q<vec2i(0)) || any(q>=vec2i(size))) { return fallback; }
  return imagePixels[offset+u32(q.y)*size.x+u32(q.x)];
}
fn imageLevel(offset0: u32, size0: vec2u, level: u32, uv: vec2f, address: vec2u, linear: bool, fallback: vec4f) -> vec4f {
  var offset=offset0; var size=size0;
  for(var l=0u;l<level;l++) { offset+=size.x*size.y; size=max(vec2u(1),size/2u); }
  // Bound coordinates before float-to-integer conversion (including extreme UVs).
  var st=uv;
  for(var a=0u;a<2u;a++) {
    if(address[a]==2u) { st[a]=st[a]-floor(st[a]); }
    else if(address[a]==3u) { st[a]=1.0-abs(1.0-(st[a]-2.0*floor(st[a]/2.0))); }
    else { st[a]=clamp(st[a],-1.0,2.0); }
  }
  let p=st*vec2f(size)-0.5; let q=vec2i(floor(p)); let f=fract(p);
  if(!linear) { return imageTexel(offset,size,vec2i(floor(p+0.5)),address,fallback); }
  return mix(mix(imageTexel(offset,size,q,address,fallback),imageTexel(offset,size,q+vec2i(1,0),address,fallback),f.x),mix(imageTexel(offset,size,q+vec2i(0,1),address,fallback),imageTexel(offset,size,q+vec2i(1,1),address,fallback),f.x),f.y);
}
fn imageSample(offset: u32, size: vec2u, levels: u32, uv: vec2f, lod: f32, address: vec2u, linear: bool, fallback: vec4f) -> vec4f {
  let l=clamp(lod,0.0,f32(levels-1u));
  if(!linear) { return imageLevel(offset,size,u32(round(l)),uv,address,false,fallback); }
  return mix(imageLevel(offset,size,u32(floor(l)),uv,address,true,fallback),imageLevel(offset,size,u32(ceil(l)),uv,address,true,fallback),fract(l));
}`;
