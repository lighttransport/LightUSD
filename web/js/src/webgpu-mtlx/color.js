// SPDX-License-Identifier: Apache-2.0
// MaterialX 1.39.5 cmlib NG_acescg_to_lin_rec709_color3, including white adaptation.
// This converts source data; the renderer's current working space remains Rec.709.
export function normalizeColorSpace(colorspace = 'lin_rec709') {
  if (typeof colorspace !== 'string' || !colorspace.trim()) throw new Error(`Unsupported color space (colorspace): ${colorspace}`);
  const key = colorspace.trim().toLowerCase();
  const aliases = {
    lin_rec709_scene: 'lin_rec709', lin_rec709: 'lin_rec709',
    srgb_rec709_scene: 'srgb_texture', srgb_texture: 'srgb_texture', srgb: 'srgb_texture',
    lin_ap1_scene: 'acescg', acescg: 'acescg',
    data: 'raw', raw: 'raw'
  };
  const normalized = aliases[key];
  if (!normalized) throw new Error(`Unsupported color space (colorspace): ${colorspace}`);
  return normalized;
}

export function colorToLinearRec709(value, colorspace = 'lin_rec709') {
  const color=Array.from(value);
  if(![3,4].includes(color.length)||!color.every(Number.isFinite))throw new Error('Expected finite RGB or RGBA color');
  const space=normalizeColorSpace(colorspace);
  if(space==='acescg') {
    const [r,g,b]=color;
    color[0]=1.705050992658*r-.621792120657*g-.083258872001*b;
    color[1]=-.130256417507*r+1.140804736575*g-.010548319068*b;
    color[2]=-.024003356805*r-.128968976065*g+1.15297233287*b;
  } else if(space==='srgb_texture') {
    for(let i=0;i<3;i++){const c=color[i];color[i]=c<=.04045?c/12.92:((c+.055)/1.055)**2.4;}
  }
  if(!color.every(Number.isFinite))throw new Error('Color transform overflow');
  return color;
}
