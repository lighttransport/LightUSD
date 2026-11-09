import * as THREE from 'three';
import { LINEAR_COLOR_SPACES, linearColorTransformMatrix as sharedLinearColorTransformMatrix, transformLinearColor as sharedTransformLinearColor } from './ColorSpaceMath.js';

const LINEAR_SPACES = LINEAR_COLOR_SPACES;

const ALIASES = Object.freeze({
  acescg: 'lin_ap1_scene', lin_acescg: 'lin_ap1_scene',
  lin_ap1: 'lin_ap1_scene',
  'aces2065-1': 'lin_ap0_scene', lin_ap0: 'lin_ap0_scene',
  lin_srgb: 'lin_rec709_scene', lin_rec709: 'lin_rec709_scene',
  linear: 'lin_rec709_scene', srgb: 'srgb_rec709_scene',
  sRGB: 'srgb_rec709_scene', srgb_texture: 'srgb_rec709_scene',
  lin_displayp3: 'lin_p3d65_scene', srgb_displayp3: 'srgb_p3d65_scene',
  lin_rec2020: 'lin_rec2020_scene', lin_adobergb: 'lin_adobergb_scene',
  adobergb: 'g22_adobergb_scene'
});

export function canonicalColorSpace(token) {
  const value = String(token || '');
  return ALIASES[value] || value;
}

export function linearColorTransformMatrix(source = 'lin_ap0_scene',
  destination = 'lin_rec709_scene') {
  source = canonicalColorSpace(source);
  destination = canonicalColorSpace(destination);
  return sharedLinearColorTransformMatrix(source, destination);
}

export function transformLinearColor(rgb, source = 'lin_ap0_scene',
  destination = 'lin_rec709_scene') {
  return sharedTransformLinearColor(rgb, canonicalColorSpace(source), canonicalColorSpace(destination));
}

// Describe the operations Three.js must perform after fetching a color texel.
// sRGB transfer decoding is delegated to Three/WebGL; other gamma curves and
// all gamut transforms are injected into the material shader.
export function textureColorTransform(token,
  destination = 'lin_rec709_scene', resolved = null) {
  const canonical = canonicalColorSpace(token);
  const resolvedMatrix = resolved?.sourceToDisplayLinear;
  if (resolved?.colorTransformValid && !resolved.colorTransformApplied &&
      Array.isArray(resolvedMatrix) &&
      resolvedMatrix.length === 9) {
    const isData = !!resolved.sourceColorIsData;
    const authoredGamma = Number(resolved.sourceGamma ?? 1);
    const authoredBias = Number(resolved.sourceLinearBias ?? 0);
    // WebGL/Three can perform the standard sRGB EOTF in the texture sampler.
    // Keep custom primaries as a shader matrix while avoiding a second EOTF.
    const hardwareSrgb = !isData && Math.abs(authoredGamma - 2.4) < 1e-6 &&
      Math.abs(authoredBias - 0.055) < 1e-6;
    const gamma = hardwareSrgb ? 1 : authoredGamma;
    const linearBias = hardwareSrgb ? 0 : authoredBias;
    const matrix = Array.from(resolvedMatrix, Number);
    const identity = matrix.every((value, index) =>
      Math.abs(value - (index % 4 === 0 ? 1 : 0)) < 1e-8);
    return {
      canonical: canonical || String(token || ''),
      colorRole: hardwareSrgb ? 'color' : 'data',
      gamma,
      linearBias,
      matrix,
      bypass: !!resolved.colorTransformBypass ||
        (identity && Math.abs(gamma - 1) < 1e-8)
    };
  }
  if (!canonical || canonical === 'auto' || canonical === 'unknown') return null;
  if (canonical === 'raw' || canonical === 'data' || canonical === 'identity') {
    return { canonical, colorRole: 'data', gamma: 1, linearBias: 0,
      matrix: [1, 0, 0, 0, 1, 0, 0, 0, 1], bypass: true };
  }
  let linear = canonical;
  let colorRole = 'data';
  let gamma = 1;
  const replacements = {
    srgb_rec709_scene: 'lin_rec709_scene',
    srgb_ap1_scene: 'lin_ap1_scene',
    srgb_p3d65_scene: 'lin_p3d65_scene',
    g22_rec709_scene: 'lin_rec709_scene',
    g18_rec709_scene: 'lin_rec709_scene',
    g22_ap1_scene: 'lin_ap1_scene',
    g22_adobergb_scene: 'lin_adobergb_scene'
  };
  if (canonical.startsWith('srgb_')) colorRole = 'color';
  if (canonical.startsWith('g22_')) gamma = 2.2;
  if (canonical.startsWith('g18_')) gamma = 1.8;
  linear = replacements[canonical] || canonical;
  if (!LINEAR_SPACES[linear]) return null;
  const matrix = linearColorTransformMatrix(linear, destination);
  const identity = matrix.every((value, index) =>
    Math.abs(value - (index % 4 === 0 ? 1 : 0)) < 1e-8);
  return { canonical, linear, colorRole, gamma, linearBias: 0, matrix,
    bypass: identity && gamma === 1 };
}

function glslFloat(value) {
  const finite = Number.isFinite(value) ? value : 0;
  if (Math.abs(finite) < 1e-12) return '0.0';
  const text = finite.toPrecision(10);
  return /[.eE]/.test(text) ? text : `${text}.0`;
}

function textureTransformExpression(value, transform) {
  let expression = value;
  if (Math.abs((transform?.gamma ?? 1) - 1) > 1e-8) {
    const gamma = Number(transform.gamma);
    const bias = Number(transform.linearBias ?? 0);
    if (bias > 0) {
      const k0 = bias / (gamma - 1);
      const phi = (bias /
        Math.exp(Math.log(gamma * bias /
          (gamma + gamma * bias - 1 - bias)) * gamma)) /
        (gamma - 1);
      const absValue = `abs(${expression})`;
      const linear = `(${absValue} / vec3(${glslFloat(phi)}))`;
      const power = `pow((${absValue} + vec3(${glslFloat(bias)})) / ` +
        `vec3(${glslFloat(1 + bias)}), vec3(${glslFloat(gamma)}))`;
      expression = `(sign(${expression}) * mix(${linear}, ${power}, ` +
        `step(vec3(${glslFloat(k0)}), ${absValue})))`;
    } else {
      expression = `(sign(${expression}) * pow(abs(${expression}), vec3(${glslFloat(gamma)})))`;
    }
  }
  const m = transform?.matrix;
  if (Array.isArray(m) && m.length === 9) {
    // GLSL mat3 constructors are column-major; the shared transform is row-major.
    const columns = [m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]];
    expression = `(mat3(${columns.map(glslFloat).join(', ')}) * ${expression})`;
  }
  return expression;
}

// Install per-color-map source-to-display conversion without resampling the
// texture through an 8-bit canvas (which would clamp AP0/AP1 out-of-gamut
// values). Three.js handles sRGB EOTF from texture.colorSpace; this hook adds
// other gamma curves and the linear-primary matrix in the fragment shader.
export function installTextureColorTransform(material, mapProperty, transform) {
  if (!material || !transform || transform.bypass ||
      (mapProperty !== 'map' && mapProperty !== 'emissiveMap')) return;
  const transforms = material.userData.lightusdTextureColorTransforms || {};
  transforms[mapProperty] = transform;
  material.userData.lightusdTextureColorTransforms = transforms;
  if (material.userData.lightusdTextureColorHookInstalled) return;
  material.userData.lightusdTextureColorHookInstalled = true;
  const previousCompile = material.onBeforeCompile?.bind(material);
  const previousCacheKey = material.customProgramCacheKey?.bind(material);
  material.onBeforeCompile = (shader, renderer) => {
    if (previousCompile) previousCompile(shader, renderer);
    const configured = material.userData.lightusdTextureColorTransforms || {};
    if (configured.map) {
      const chunk = THREE.ShaderChunk.map_fragment.replace(
        'diffuseColor *= sampledDiffuseColor;',
        `sampledDiffuseColor.rgb = ${textureTransformExpression(
          'sampledDiffuseColor.rgb', configured.map)};\n\tdiffuseColor *= sampledDiffuseColor;`);
      shader.fragmentShader = shader.fragmentShader.replace(
        '#include <map_fragment>', chunk);
    }
    if (configured.emissiveMap) {
      const chunk = THREE.ShaderChunk.emissivemap_fragment.replace(
        'totalEmissiveRadiance *= emissiveColor.rgb;',
        `emissiveColor.rgb = ${textureTransformExpression(
          'emissiveColor.rgb', configured.emissiveMap)};\n\ttotalEmissiveRadiance *= emissiveColor.rgb;`);
      shader.fragmentShader = shader.fragmentShader.replace(
        '#include <emissivemap_fragment>', chunk);
    }
  };
  material.customProgramCacheKey = () => {
    const prior = previousCacheKey ? previousCacheKey() : '';
    return `${prior}|lightusd-cs:${JSON.stringify(
      material.userData.lightusdTextureColorTransforms)}`;
  };
}
