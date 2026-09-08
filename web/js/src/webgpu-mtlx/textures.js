// SPDX-License-Identifier: Apache-2.0
// Decoded image resources: RGBA float data, row zero at MaterialX v=0.
// Preview working space is linear Rec.709; ACEScg transport is a later milestone.
import { normalizeColorSpace } from './color.js';
function resizeBox(image, maxDimension) {
  if (!maxDimension || Math.max(image.width, image.height) <= maxDimension) return image;
  const scale = maxDimension / Math.max(image.width, image.height);
  const width = Math.max(1, Math.floor(image.width * scale)), height = Math.max(1, Math.floor(image.height * scale));
  const data = new Float32Array(width * height * 4);
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const x0 = x * image.width / width, x1 = (x + 1) * image.width / width;
    const y0 = y * image.height / height, y1 = (y + 1) * image.height / height;
    let sum = 0;
    for (let iy = Math.floor(y0); iy < Math.ceil(y1); iy++) for (let ix = Math.floor(x0); ix < Math.ceil(x1); ix++) {
      const weight = (Math.min(ix + 1, x1) - Math.max(ix, x0)) * (Math.min(iy + 1, y1) - Math.max(iy, y0));
      const src = (iy * image.width + ix) * 4, dst = (y * width + x) * 4;
      for (let c = 0; c < 4; c++) data[dst + c] += image.data[src + c] * weight;
      sum += weight;
    }
    const dst = (y * width + x) * 4; for (let c = 0; c < 4; c++) data[dst + c] /= sum;
  }
  return { ...image, width, height, data, resizedFrom: [image.width, image.height] };
}

export function packImages(images, { maxBytes = 64 * 1024 * 1024, maxDimension } = {}) {
  const chunks = [], descriptors = []; let texels = 0;
  const packOne = source => {
    const image = resizeBox(source, maxDimension);
    const { width, height, data } = image;
    const colorspace = normalizeColorSpace(image.colorspace || 'lin_rec709');
    if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width > 16384 || height > 16384) throw new Error('Invalid image dimensions');
    if (!data || data.length !== width * height * 4) throw new Error('Expected RGBA image data');
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
    const descriptor = { offset: texels, width, height, levels: 0,
      ...(image.resizedFrom ? { resizedFrom: image.resizedFrom } : {}),
      ...(image.udim ? { udim: { ...image.udim } } : {}) };
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
    return descriptor;
  };
  for (const image of images) {
    if (Array.isArray(image?.layers)) {
      if (Array.isArray(image.frames)) throw new Error('Image resources cannot combine layers and frames');
      if (!image.layers.length || image.layers.length > 64) throw new Error('Image layer count exceeds budget');
      const layers = [packOne(image), ...image.layers.map(layer => packOne({ ...layer, colorspace: layer.colorspace ?? image.colorspace }))];
      const first = layers[0];
      if (layers.some(layer => layer.width !== first.width || layer.height !== first.height || layer.levels !== first.levels)) throw new Error('Image layers must have matching dimensions');
      descriptors.push({ ...first, layers });
      continue;
    }
    if (!Array.isArray(image?.frames)) { descriptors.push(packOne(image)); continue; }
    if (!image.frames.length || image.frames.length > 1024) throw new Error('Image sequence must contain 1..1024 frames');
    const frames = image.frames.map(frame => packOne({ ...frame, colorspace: frame.colorspace ?? image.colorspace }));
    const first = frames[0];
    if (frames.some(frame => frame.width !== first.width || frame.height !== first.height || frame.levels !== first.levels)) throw new Error('Image sequence frames must have matching dimensions');
    descriptors.push({ ...first, frames });
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
fn imageCubicWeight(x:f32)->f32 { let a=abs(x); if(a<=1.0){return 1.5*a*a*a-2.5*a*a+1.0;} if(a<2.0){return -0.5*a*a*a+2.5*a*a-4.0*a+2.0;} return 0.0; }
fn imageCubicLevel(offset0:u32,size0:vec2u,level:u32,uv:vec2f,address:vec2u,fallback:vec4f)->vec4f {
  var offset=offset0;var size=size0;for(var l=0u;l<level;l++){offset+=size.x*size.y;size=max(vec2u(1),size/2u);}
  var st=uv;for(var a=0u;a<2u;a++){if(address[a]==2u){st[a]=st[a]-floor(st[a]);}else if(address[a]==3u){st[a]=1.0-abs(1.0-(st[a]-2.0*floor(st[a]/2.0)));}else{st[a]=clamp(st[a],-1.0,2.0);}}
  let p=st*vec2f(size)-0.5;let q=vec2i(floor(p));var result=vec4f(0);var total=0.0;
  for(var y=-1;y<=2;y++){let wy=imageCubicWeight(f32(y)-fract(p.y));for(var x=-1;x<=2;x++){let w=wy*imageCubicWeight(f32(x)-fract(p.x));result+=imageTexel(offset,size,q+vec2i(x,y),address,fallback)*w;total+=w;}}
  return result/max(1e-6,total);
}
fn imageSampleCubic(offset:u32,size:vec2u,levels:u32,uv:vec2f,lod:f32,address:vec2u,fallback:vec4f)->vec4f {
  let l=clamp(lod,0.0,f32(levels-1u));return mix(imageCubicLevel(offset,size,u32(floor(l)),uv,address,fallback),imageCubicLevel(offset,size,u32(ceil(l)),uv,address,fallback),fract(l));
}
fn imageSample(offset: u32, size: vec2u, levels: u32, uv: vec2f, lod: f32, address: vec2u, linear: bool, fallback: vec4f) -> vec4f {
  let l=clamp(lod,0.0,f32(levels-1u));
  if(!linear) { return imageLevel(offset,size,u32(round(l)),uv,address,false,fallback); }
  return mix(imageLevel(offset,size,u32(floor(l)),uv,address,true,fallback),imageLevel(offset,size,u32(ceil(l)),uv,address,true,fallback),fract(l));
}
fn imageSampleUDIM(offset:u32,size:vec2u,levels:u32,uv:vec2f,grid:vec2u,lod:f32,linear:bool,fallback:vec4f)->vec4f {
  let tile=clamp(floor(uv),vec2f(0.0),vec2f(grid)-vec2f(1.0));
  let local=clamp(fract(uv),vec2f(0.5)/vec2f(size/grid),vec2f(1.0)-vec2f(0.5)/vec2f(size/grid));
  return imageSample(offset,size,levels,(tile+local)/vec2f(grid),lod,vec2u(1u),linear,fallback);
}
fn imageSampleCubicUDIM(offset:u32,size:vec2u,levels:u32,uv:vec2f,grid:vec2u,lod:f32,fallback:vec4f)->vec4f {
  let tile=clamp(floor(uv),vec2f(0.0),vec2f(grid)-vec2f(1.0));
  let local=clamp(fract(uv),vec2f(0.5)/vec2f(size/grid),vec2f(1.0)-vec2f(0.5)/vec2f(size/grid));
  return imageSampleCubic(offset,size,levels,(tile+local)/vec2f(grid),lod,vec2u(1u),fallback);
}
fn imageSequenceIndex(frame:f32,start:f32,end:f32,offset:f32,count:u32,action:u32)->u32 {
  let raw=floor(frame+offset-start);let n=max(1u,count);
  if(action==1u){let i=i32(raw);let m=i32(n);return u32((i%m+m)%m);}
  if(action==2u&&n>1u){let period=2u*n-2u;let i=u32((i32(raw)%i32(period)+i32(period))%i32(period));return select(i,period-i,i>=n);}
  return u32(clamp(raw,0.0,f32(n-1u)));
}
fn mxHextileHash(p:vec2f)->vec2f {
  var p3=fract(vec3f(p.x,p.y,p.x)*vec3f(0.1031,0.1030,0.0973));
  p3+=dot(p3,vec3f(p3.y,p3.z,p3.x)+33.33);
  return fract((vec2f(p3.x,p3.x)+vec2f(p3.y,p3.z))*vec2f(p3.z,p3.y));
}
fn mxHextileGain(x0:f32,r0:f32)->f32 {
  let r=clamp(r0,0.001,0.999);let a=(1.0/r-2.0)*(1.0-2.0*x0);
  return select((a-x0)/(a-1.0),x0/(a+1.0),x0<0.5);
}
struct MxHextileData { coords:array<vec2f,3>, ddx:array<vec2f,3>, ddy:array<vec2f,3>, weights:vec3f, rotations:vec3f }
fn mxHextileCoord(coord:vec2f,ddx0:vec2f,ddy0:vec2f,rotation:f32,rotationRange:vec2f,scale:f32,scaleRange:vec2f,offset:f32,offsetRange:vec2f)->MxHextileData {
  let s32=sqrt(3.0)*2.0;let st=coord*s32;let skew=vec2f(st.x-0.57735027*st.y,1.15470054*st.y);let fracv=fract(skew);let z=1.0-fracv.x-fracv.y;let s=select(0.0,1.0,z<=0.0);let s2=2.0*s-1.0;
  let weights=vec3f(-z*s2,s-fracv.y*s2,s-fracv.x*s2);let base=floor(skew);let id1=base+vec2f(s);let id2=base+vec2f(s,1.0-s);let id3=base+vec2f(1.0-s,s);
  let invS32=1.0/s32;let ctr1=vec2f(id1.x+0.5*id1.y,id1.y/1.15470054)*invS32;let ctr2=vec2f(id2.x+0.5*id2.y,id2.y/1.15470054)*invS32;let ctr3=vec2f(id3.x+0.5*id3.y,id3.y/1.15470054)*invS32;
  let r1=mxHextileHash(id1+vec2f(0.12345));let r2=mxHextileHash(id2+vec2f(0.12345));let r3=mxHextileHash(id3+vec2f(0.12345));let rr=rotationRange*0.017453292519943295;let rot=mix(vec3f(rr.x),vec3f(rr.y),vec3f(r1.x,r2.x,r3.x)*rotation);let sn=sin(rot);let cs=cos(rot);
  let scales=mix(vec3f(1.0),mix(vec3f(scaleRange.x),vec3f(scaleRange.y),vec3f(r1.y,r2.y,r3.y)),vec3f(scale));let o1=mix(vec2f(offsetRange.x),vec2f(offsetRange.y),r1*offset);let o2=mix(vec2f(offsetRange.x),vec2f(offsetRange.y),r2*offset);let o3=mix(vec2f(offsetRange.x),vec2f(offsetRange.y),r3*offset);
  let q1=coord-ctr1;let q2=coord-ctr2;let q3=coord-ctr3;let d1=vec2f(cs.x*q1.x-sn.x*q1.y,sn.x*q1.x+cs.x*q1.y)/scales.x;let d2=vec2f(cs.y*q2.x-sn.y*q2.y,sn.y*q2.x+cs.y*q2.y)/scales.y;let d3=vec2f(cs.z*q3.x-sn.z*q3.y,sn.z*q3.x+cs.z*q3.y)/scales.z;
  var result:MxHextileData;result.coords=array<vec2f,3>(d1+ctr1+o1,d2+ctr2+o2,d3+ctr3+o3);result.ddx=array<vec2f,3>(vec2f(cs.x*ddx0.x-sn.x*ddx0.y,sn.x*ddx0.x+cs.x*ddx0.y)/scales.x,vec2f(cs.y*ddx0.x-sn.y*ddx0.y,sn.y*ddx0.x+cs.y*ddx0.y)/scales.y,vec2f(cs.z*ddx0.x-sn.z*ddx0.y,sn.z*ddx0.x+cs.z*ddx0.y)/scales.z);result.ddy=array<vec2f,3>(vec2f(cs.x*ddy0.x-sn.x*ddy0.y,sn.x*ddy0.x+cs.x*ddy0.y)/scales.x,vec2f(cs.y*ddy0.x-sn.y*ddy0.y,sn.y*ddy0.x+cs.y*ddy0.y)/scales.y,vec2f(cs.z*ddy0.x-sn.z*ddy0.y,sn.z*ddy0.x+cs.z*ddy0.y)/scales.z);result.weights=weights;result.rotations=rot;return result;
}
fn mxHextileBlendWeights(luma:vec3f,tileWeights:vec3f,falloff:f32)->vec3f {
  var w=luma*pow(max(tileWeights,vec3f(0.0)),vec3f(7.0));w/=max(1e-6,w.x+w.y+w.z);
  if(abs(falloff-0.5)>1e-6){w=vec3f(mxHextileGain(w.x,falloff),mxHextileGain(w.y,falloff),mxHextileGain(w.z,falloff));w/=max(1e-6,w.x+w.y+w.z);}return w;
}
fn imageHextile(offset:u32,size:vec2u,levels:u32,coord:vec2f,ddx0:vec2f,ddy0:vec2f,rotation:f32,rotationRange:vec2f,scale:f32,scaleRange:vec2f,offsetAmount:f32,offsetRange:vec2f,falloff:f32,falloffContrast:f32,lumacoeffs:vec3f,address:vec2u,linear:bool,fallback:vec4f)->vec4f {
  let t=mxHextileCoord(coord,ddx0,ddy0,rotation,rotationRange,scale,scaleRange,offsetAmount,offsetRange);let lod0=log2(max(1.0,max(length(t.ddx[0]*vec2f(size)),length(t.ddy[0]*vec2f(size)))));let lod1=log2(max(1.0,max(length(t.ddx[1]*vec2f(size)),length(t.ddy[1]*vec2f(size)))));let lod2=log2(max(1.0,max(length(t.ddx[2]*vec2f(size)),length(t.ddy[2]*vec2f(size)))));
  let c1=imageSample(offset,size,levels,t.coords[0],lod0,address,linear,fallback);let c2=imageSample(offset,size,levels,t.coords[1],lod1,address,linear,fallback);let c3=imageSample(offset,size,levels,t.coords[2],lod2,address,linear,fallback);let cw=vec3f(dot(c1.rgb,lumacoeffs),dot(c2.rgb,lumacoeffs),dot(c3.rgb,lumacoeffs));let w=mxHextileBlendWeights(mix(vec3f(1.0),cw,vec3f(falloffContrast)),t.weights,falloff);let aw=mxHextileBlendWeights(vec3f(1.0),t.weights,falloff);return vec4f(w.x*c1.rgb+w.y*c2.rgb+w.z*c3.rgb,aw.x*c1.a+aw.y*c2.a+aw.z*c3.a);
}
fn mxHextileAxisRotate(a:vec3f,v:vec3f,r:f32)->vec3f { let s=sin(r);let c=cos(r);return v*c+cross(a,v)*s+a*dot(a,v)*(1.0-c); }
fn mxHextileNormalGradient(N:vec3f,Np:vec3f)->vec3f { let d=dot(N,Np);return (d*N-Np)/max(1e-6,abs(d)); }
fn imageHextileNormal(offset:u32,size:vec2u,levels:u32,coord:vec2f,ddx0:vec2f,ddy0:vec2f,rotation:f32,rotationRange:vec2f,scale:f32,scaleRange:vec2f,offsetAmount:f32,offsetRange:vec2f,falloff:f32,strength:f32,flipG:bool,N:vec3f,T:vec3f,B:vec3f,address:vec2u,linear:bool,fallback:vec4f)->vec3f {
  let t=mxHextileCoord(coord,ddx0,ddy0,rotation,rotationRange,scale,scaleRange,offsetAmount,offsetRange);let lod0=log2(max(1.0,max(length(t.ddx[0]*vec2f(size)),length(t.ddy[0]*vec2f(size)))));let lod1=log2(max(1.0,max(length(t.ddx[1]*vec2f(size)),length(t.ddy[1]*vec2f(size)))));let lod2=log2(max(1.0,max(length(t.ddx[2]*vec2f(size)),length(t.ddy[2]*vec2f(size)))));
  var n1=imageSample(offset,size,levels,t.coords[0],lod0,address,linear,fallback).rgb;var n2=imageSample(offset,size,levels,t.coords[1],lod1,address,linear,fallback).rgb;var n3=imageSample(offset,size,levels,t.coords[2],lod2,address,linear,fallback).rgb;if(flipG){n1.y=1.0-n1.y;n2.y=1.0-n2.y;n3.y=1.0-n3.y;}n1=2.0*n1-vec3f(1.0);n2=2.0*n2-vec3f(1.0);n3=2.0*n3-vec3f(1.0);
  let n=safeNormal(N,vec3f(0.0,0.0,1.0));let r1=-t.rotations.x;let r2=-t.rotations.y;let r3=-t.rotations.z;let t1=mxHextileAxisRotate(n,T,r1)*strength;let t2=mxHextileAxisRotate(n,T,r2)*strength;let t3=mxHextileAxisRotate(n,T,r3)*strength;let b1=mxHextileAxisRotate(n,B,r1)*strength;let b2=mxHextileAxisRotate(n,B,r2)*strength;let b3=mxHextileAxisRotate(n,B,r3)*strength;let n1w=safeNormal(t1*n1.x+b1*n1.y+n*n1.z,n);let n2w=safeNormal(t2*n2.x+b2*n2.y+n*n2.z,n);let n3w=safeNormal(t3*n3.x+b3*n3.y+n*n3.z,n);let w=mxHextileBlendWeights(vec3f(1.0),t.weights,falloff);let g=w.x*mxHextileNormalGradient(n,n1w)+w.y*mxHextileNormalGradient(n,n2w)+w.z*mxHextileNormalGradient(n,n3w);return safeNormal(n-g,n);
}
`;
