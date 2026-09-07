// SPDX-License-Identifier: Apache-2.0
import { cieXYZ } from './cie-data.js';
import * as basis from './rgb-spectrum-data.js';
export const CIE_Y_INTEGRAL = 106.856915;
export function validateSpectrum(points, { ior = false } = {}) {
  if (!Array.isArray(points) || points.length < 2 || points.length > 256) throw new Error('Spectrum requires 2..256 wavelength/value pairs');
  let previous = -Infinity;
  for (const pair of points) {
    if (!Array.isArray(pair) || pair.length !== 2 || !pair.every(v => Number.isFinite(v) && Number.isFinite(Math.fround(v))) || pair[0] <= previous || pair[1] < 0 || (ior && pair[1] <= 0)) throw new Error('Invalid or unordered spectrum');
    previous = pair[0];
  }
  if (ior && (points[0][0] > 360 || points.at(-1)[0] < 830)) throw new Error('IOR spectrum must cover 360..830 nm');
  return points;
}
export function sampleSpectrum(points, wavelength) {
  if (wavelength < points[0][0] || wavelength > points.at(-1)[0]) return 0;
  for (let i = 1; i < points.length; i++) if (wavelength <= points[i][0]) {
    const t = (wavelength - points[i-1][0]) / (points[i][0] - points[i-1][0]);
    return points[i-1][1] * (1-t) + points[i][1] * t;
  }
  return points.at(-1)[1];
}
const f = n => `${Number(n).toExponential(12)}`;
// Normalize the illuminant white basis to Y=1 over our full 360..830 nm range.
// The legacy PBRT scale targets its discretization and otherwise gives Y~0.921.
const whitePoints = basis.RGB2SpectLambda.map((l,i)=>[l,basis.RGBIllum2SpectWhite[i]]);
const whiteAt = l => l < 380 ? whitePoints[0][1] : l > 720 ? whitePoints.at(-1)[1] : sampleSpectrum(whitePoints,l);
export const ILLUMINANT_SCALE = CIE_Y_INTEGRAL / cieXYZ.slice(0,-1).reduce((sum,row,i)=>sum+(row[1]+cieXYZ[i+1][1])*.5*whiteAt(360.5+i),0);
export function spectrumWGSL(materials, resources = {}) {
  const data = [basis.RGB2SpectLambda, ...['Refl', 'Illum'].flatMap(kind => ['White','Cyan','Magenta','Yellow','Red','Green','Blue'].map(color => basis[`RGB${kind}2Spect${color}`]))];
  resources.spectralData = new Float32Array([...data.flat(), ...cieXYZ.flat()]);
  let code = `@group(0) @binding(8) var<storage,read> spectralData: array<f32>;\nfn spectralBasis(i:u32)->f32 {return spectralData[i];}\nfn cieTable(i:u32)->vec3f {let p=480u+i*3u;return vec3f(spectralData[p],spectralData[p+1u],spectralData[p+2u]);}\n`;
  code += /* wgsl */`
fn cieAt(lambda: f32) -> vec3f {
  let x=clamp(lambda-360.0,0.0,470.0); let i=u32(floor(x));
  return mix(cieTable(i),cieTable(min(470u,i+1u)),x-f32(i));
}
fn basisAt(index: u32, lambda: f32) -> f32 {
  // Basis data spans 380..720 nm; endpoint extension matches PBRT v3.
  if(lambda<=spectralBasis(0u)) { return spectralBasis(index*32u); }
  for(var i=1u;i<32u;i++) { if(lambda<=spectralBasis(i)) {
    let t=(lambda-spectralBasis(i-1u))/(spectralBasis(i)-spectralBasis(i-1u));
    return mix(spectralBasis(index*32u+i-1u),spectralBasis(index*32u+i),t);
  } }
  return spectralBasis(index*32u+31u);
}
fn rgbSpectrum(rgb0: vec3f, lambda: f32, illuminant: bool) -> f32 {
  let rgb=max(vec3f(0),rgb0); let b=select(1u,8u,illuminant); var value=0.0;
  if(rgb.x<=rgb.y && rgb.x<=rgb.z) {
    value=rgb.x*basisAt(b,lambda);
    if(rgb.y<=rgb.z) { value+=(rgb.y-rgb.x)*basisAt(b+1u,lambda)+(rgb.z-rgb.y)*basisAt(b+6u,lambda); }
    else { value+=(rgb.z-rgb.x)*basisAt(b+1u,lambda)+(rgb.y-rgb.z)*basisAt(b+5u,lambda); }
  } else if(rgb.y<=rgb.x && rgb.y<=rgb.z) {
    value=rgb.y*basisAt(b,lambda);
    if(rgb.x<=rgb.z) { value+=(rgb.x-rgb.y)*basisAt(b+2u,lambda)+(rgb.z-rgb.x)*basisAt(b+6u,lambda); }
    else { value+=(rgb.z-rgb.y)*basisAt(b+2u,lambda)+(rgb.x-rgb.z)*basisAt(b+4u,lambda); }
  } else {
    value=rgb.z*basisAt(b,lambda);
    if(rgb.x<=rgb.y) { value+=(rgb.x-rgb.z)*basisAt(b+3u,lambda)+(rgb.y-rgb.x)*basisAt(b+5u,lambda); }
    else { value+=(rgb.y-rgb.z)*basisAt(b+3u,lambda)+(rgb.x-rgb.y)*basisAt(b+4u,lambda); }
  }
  return max(0.0,value*select(0.94,${f(ILLUMINANT_SCALE)},illuminant));
}
fn xyzToRGB(xyz: vec3f) -> vec3f {
  return mat3x3f(3.2404542,-0.9692660,0.0556434,-1.5371385,1.8760108,-0.2040259,-0.4985314,0.0415560,1.0572252)*xyz;
}
`;
  const cases = [];
  materials.forEach((doc, i) => {
    const statements = [];
    for (const [field, points] of Object.entries(doc.spectra || {})) {
      const target = ({ base_color: 'base', transmission_color: 'transmissionColor', emission_color: 'emission', ior: 'ior', conductor_ior: 'complexIOR', extinction: 'extinction' })[field];
      if (!target) throw new Error(`Unsupported spectral override ${field}`);
      validateSpectrum(points, { ior: field === 'ior' });
      const name = `measured_${i}_${target}`;
      code += `const ${name}=array<vec2f,${points.length}>(${points.map(p => `vec2f(${p.map(f).join(',')})`).join(',')});\n`;
      code += `fn ${name}_at(lambda:f32)->f32 { if(lambda<${name}[0].x || lambda>${name}[${points.length-1}].x) { return 0.0; } for(var i=1u;i<${points.length}u;i++) { if(lambda<=${name}[i].x) { let a=${name}[i-1u];let b=${name}[i];return mix(a.y,b.y,(lambda-a.x)/(b.x-a.x)); } } return ${name}[${points.length-1}].y; }\n`;
      statements.push(`m.${target}=${field === 'ior' ? `${name}_at(lambda)` : `vec3f(${name}_at(lambda))`};`);
    }
    cases.push(`case ${i}u: { ${statements.join('\n')} }`);
  });
  code += `fn spectralMaterial(input:Lobe,id:u32,lambda:f32)->Lobe { var m=input;m.base=vec3f(rgbSpectrum(m.base,lambda,false));m.schlickColor90=vec3f(rgbSpectrum(m.schlickColor90,lambda,false));m.transmissionColor=vec3f(rgbSpectrum(m.transmissionColor,lambda,false));m.transmissionScatter=vec3f(rgbSpectrum(m.transmissionScatter,lambda,false));m.emission=vec3f(rgbSpectrum(m.emission,lambda,true));m.complexIOR=vec3f(rgbSpectrum(m.complexIOR,lambda,false));m.extinction=vec3f(rgbSpectrum(m.extinction,lambda,false));switch id { ${cases.join('\n')} default:{} }return m; }\n`;
  code += `fn spectralSurface(input:Material,id:u32,lambda:f32)->Material {var s=input;let primary=spectralMaterial(primaryLobe(input),id,lambda);s.emission=primary.emission;for(var i=0u;i<s.bsdf.count;i++){s.bsdf.lobes[i]=spectralMaterial(s.bsdf.lobes[i],id,lambda);let scale=s.bsdf.scales[i];if(scale.x==scale.y&&scale.y==scale.z){s.bsdf.scales[i]=vec3f(scale.x);}else{s.bsdf.scales[i]=vec3f(rgbSpectrum(scale,lambda,false)/max(1e-8,rgbSpectrum(vec3f(1),lambda,false)));}}return s;}\n`;
  return code;
}
