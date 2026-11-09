import * as THREE from 'three';

// Rendering-state adjustments shared by the online viewer and foundation demos.
// Three.js multiplies texture inputs by scalar/color properties, while USD
// connections replace their fallback values. Keep connected textures visible.
export function applyUSDMaterialFeatures(material, mapProperty = '', {
  normalScale = 1.35,
  aoIntensity = 1.2
} = {}) {
  if (!material) return material;

  if (mapProperty === 'normalMap' || material.normalMap) {
    material.normalMapType = THREE.TangentSpaceNormalMap;
    material.normalScale?.set(normalScale, normalScale);
  }
  if (mapProperty === 'aoMap' || material.aoMap) material.aoMapIntensity = aoIntensity;
  if (mapProperty === 'alphaMap' || material.alphaMap) {
    material.transparent = true;
    material.alphaTest = Math.max(material.alphaTest || 0, 0.01);
    material.depthWrite = false;
  }
  if (mapProperty === 'emissiveMap' || material.emissiveMap) {
    if (material.emissive?.isColor && material.emissive.getHex() === 0) {
      material.emissive.setRGB(1, 1, 1);
    }
    if (!Number.isFinite(material.emissiveIntensity) || material.emissiveIntensity === 0) {
      material.emissiveIntensity = 1;
    }
  }
  material.needsUpdate = true;
  return material;
}
