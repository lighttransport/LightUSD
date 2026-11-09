import * as THREE from 'three';
import { HDRCubeTextureLoader } from 'three/addons/loaders/HDRCubeTextureLoader.js';

import { GUI } from 'https://cdn.jsdelivr.net/npm/dat.gui@0.7.9/build/dat.gui.module.js';

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js'
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js'
import { buildNextThreeNode, isNextScene } from 'lightusd/NextRenderSceneUtils.js'

const gui = new GUI();

let ui_state = {}
ui_state['rot_scale'] = 1.0;
ui_state['mtl'] = LightUSDLoaderUtils.createDefaultMaterial();

ui_state['envMapIntensity'] = 3.14; // pi is good for pisaHDR;
ui_state['ambient'] = 0.4;
let ambientLight = new THREE.AmbientLight(0x404040, ui_state['ambient']);
ui_state['camera_z'] = 4; // TODO: Compute best fit from scene's bbox.
ui_state['useWhiteEnvmap'] = true;

ui_state['shader_normal'] = false;
ui_state['diffuse_texture_enabled'] = true;
ui_state['normal_texture_enabled'] = true;
ui_state['ao_texture_enabled'] = true;
ui_state['alpha_texture_enabled'] = true;

// Default PBR mateiral params
ui_state['diffuse'] = new THREE.Color(49, 49, 49); // 0.18
ui_state['diffuseMap'] = null;
ui_state['normalMap'] = null;
ui_state['aoMap'] = null;
ui_state['alphaMap'] = null;
ui_state['emissive'] = new THREE.Color(0, 0, 0);
ui_state['roughness'] = 0.5;
ui_state['metalness'] = 0.0;
ui_state['clearcoat'] = 0.0;
ui_state['clearcoatRoughness'] = 0.0;
ui_state['ior'] = 1.5;
ui_state['specularIntensity'] = 1.0;
ui_state['opacity'] = 1.0;
ui_state['transparent'] = false;

// Create a parameters object
const params = {
  envMapIntensity: ui_state['envMapIntensity'],
  rotationSpeed: ui_state['rot_scale'],
  camera_z: ui_state['camera_z'],
  useWhiteEnvmap: ui_state['useWhiteEnvmap'],
  shader_normal: ui_state['shader_normal'],
  diffuse: ui_state['diffuse'],
  emissive: ui_state['emissive'],
  roughness: ui_state['roughness'],
  metalness: ui_state['metalness'],
  clearcoat: ui_state['clearcoat'],
  clearcoatRoughness: ui_state['clearcoatRoughness'],
  specularIntensity: ui_state['specularIntensity'],
  opacity: ui_state['opacity'],
  transparent: ui_state['transparent'],
  ior: ui_state['ior'],
  diffuse_texture_enabled: ui_state['diffuse_texture_enabled'],
  normal_texture_enabled: ui_state['normal_texture_enabled'],
  ao_texture_enabled: ui_state['ao_texture_enabled'],
  alpha_texture_enabled: ui_state['alpha_texture_enabled'],
};

// Add controls
gui.add(params, 'envMapIntensity', 0, 20, 0.1).name('envMapIntensity').onChange((value) => {
  ui_state['envMapIntensity'] = value;
});
gui.add(params, 'camera_z', 0, 20).name('Camera Z').onChange((value) => {
  ui_state['camera_z'] = value;
});
gui.add(params, 'rotationSpeed', 0, 10).name('Rotation Speed').onChange((value) => {
  ui_state['rot_scale'] = value;
});
gui.add(params, 'useWhiteEnvmap').name('Use White Envmap').onChange((value) => {
  ui_state['useWhiteEnvmap'] = value;
  updateEnvironment();
});


gui.addColor(params, 'diffuse').name('color').onChange((value) => {
  ui_state['diffuse'] = value;
  ui_state['material_changed'] = true;
});

gui.add(params, 'diffuse_texture_enabled').name('Diffuse Texture').onChange((value) => {
  ui_state['diffuse_texture_enabled'] = value;
  ui_state['material_changed'] = true;
});

gui.add(params, 'normal_texture_enabled').name('Normal Texture').onChange((value) => {
  ui_state['normal_texture_enabled'] = value;
  ui_state['material_changed'] = true;
});

gui.add(params, 'ao_texture_enabled').name('AO Texture').onChange((value) => {
  ui_state['ao_texture_enabled'] = value;
  ui_state['material_changed'] = true;
});

gui.add(params, 'alpha_texture_enabled').name('Alpha Texture').onChange((value) => {
  ui_state['alpha_texture_enabled'] = value;
  ui_state['material_changed'] = true;
});

gui.addColor(params, 'emissive').name('emissive').onChange((value) => {
  ui_state['emissive'] = value;

  ui_state['material_changed'] = true;
});

gui.add(params, 'roughness', 0.0, 1.0, 0.01).name('roughness').onChange((value) => {
  ui_state['roughness'] = value;

  ui_state['material_changed'] = true;
});

gui.add(params, 'metalness', 0.0, 1.0, 0.01).name('metalness').onChange((value) => {
  ui_state['metalness'] = value;

  ui_state['material_changed'] = true;
});

gui.add(params, 'ior', 1.0, 2.4, 0.1).name('ior').onChange((value) => {
  ui_state['ior'] = value;

  ui_state['material_changed'] = true;
});

gui.add(params, 'clearcoat', 0.0, 1.0, 0.01).name('clearcoat').onChange((value) => {
  ui_state['clearcoat'] = value;

  ui_state['material_changed'] = true;
});

gui.add(params, 'clearcoatRoughness', 0.0, 1.0).step(0.01).name('clearcoatRoughness').onChange((value) => {
  ui_state['clearcoatRoughness'] = value;

  ui_state['material_changed'] = true;
});

gui.add(params, 'specularIntensity', 0.0, 1.0, 0.01).name('specular').onChange((value) => {
  ui_state['specularIntensity'] = value;

  ui_state['material_changed'] = true;
});

gui.add(params, 'opacity', 0.0, 1.0, 0.01).name('opacity').onChange((value) => {
  ui_state['opacity'] = value;
  ui_state['material_changed'] = true;
});

gui.add(params, 'transparent').name('Transparent').onChange((value) => {
  ui_state['transparent'] = value;
  ui_state['material_changed'] = true;
});

gui.add(params, 'shader_normal').name('NormalMaterial').onChange((value) => {
  ui_state['shader_normal'] = value;

  ui_state['material_changed'] = true;
});


async function loadScenes() {

  const loader = new LightUSDLoader();

  // it is recommended to call init() before loadAsync()
  // (wait loading/compiling wasm module in the early stage))
  //await loader.init({useZstdCompressedWasm: true});
  await loader.init({useZstdCompressedWasm: false});

  const suzanne_filename = "./assets/suzanne-pbr.usda";
  const texcat_filename = "./assets/texture-cat-plane.usda";
  const cookie_filename = "./assets/UsdCookie.usdz";
  const textest_filename = "./assets/brown-rock.usdz";
  const usd_filename = "./assets/suzanne-subd-lv6.usdc";

  var threeScenes = []

  const usd_scenes = await Promise.all([
    //loader.loadAsync(texcat_filename),
    //loader.loadAsync(cookie_filename),
    //loader.loadAsync(suzanne_filename),
    loader.loadAsync(usd_filename),
  ]);

  const defaultMtl = ui_state['mtl'];

  const options = {
    overrideMaterial: false, // override USD material with defaultMtl(default 'false')
    envMap: defaultMtl.envMap, // reuse envmap from defaultMtl
    envMapIntensity: ui_state['envMapIntensity'], // default envmap intensity
  }

  var offset = -(usd_scenes.length-1) * 1.5;
  for (const usd_scene of usd_scenes) {

    let threeNode;
    if (isNextScene(usd_scene)) {
      // next (default) scenes carry next render records; build them with the
      // next scene builder and apply the shared environment map.
      const built = buildNextThreeNode(usd_scene, { skipTextures: false, lazyTextures: true });
      threeNode = built.node;
      threeNode.traverse((obj) => {
        if (!obj.isMesh) return;
        for (const mat of Array.isArray(obj.material) ? obj.material : [obj.material]) {
          if (!mat) continue;
          mat.envMap = options.envMap;
          mat.envMapIntensity = options.envMapIntensity;
          mat.needsUpdate = true;
        }
      });
      if (built.textureManager && built.textureManager.total > 0) {
        built.textureManager.startLoading({
          onTextureLoaded: (material) => { material.needsUpdate = true; }
        });
      }
    } else {
      const usdRootNode = usd_scene.getDefaultRootNode();
      threeNode = LightUSDLoaderUtils.buildThreeNode(usdRootNode, defaultMtl, usd_scene, options);
    }

    const uri = usd_scene.getURI ? usd_scene.getURI() : (usd_scene.filename || '');
    if (uri.includes('UsdCookie')) {
      // Add exra scaling
      threeNode.scale.x *= 2.5;
      threeNode.scale.y *= 2.5;
      threeNode.scale.z *= 2.5;
    }

    threeNode.position.x += offset;
    offset += 3.0;

    threeScenes.push(threeNode);
  }

  return threeScenes;

}



const scene = new THREE.Scene();



function createWhiteEnvmap() {
  // Create a simple white cube texture using HDRCubeTexture
  const size = 64; // Small size for performance
  const data = new Uint16Array(size * size * 4);

  // Fill with white color (1.0, 1.0, 1.0) for HDR
  for (let i = 0; i < data.length; i += 4) {

    data[i] = THREE.DataUtils.toHalfFloat(1.0);
    data[i+1] = THREE.DataUtils.toHalfFloat(1.0);
    data[i+2] = THREE.DataUtils.toHalfFloat(1.0);
    data[i+3] = THREE.DataUtils.toHalfFloat(1.0);
  }

  // Create individual DataTextures for each cube face using half-float
  const faces = [];
  for (let i = 0; i < 6; i++) {
    const faceTexture = new THREE.DataTexture(data, size, size); //, THREE.RGBFormat, THREE.HalfFloatType);
    faceTexture.needsUpdate = true;
    faces.push(faceTexture);
  }

  // Create HDRCubeTexture
  const cubeTexture = new THREE.CubeTexture();
  cubeTexture.image = faces.map(face => createImageFromFloatData(data, size, size));
  cubeTexture.format = THREE.RGBAFormat;
  cubeTexture.type = THREE.HalfFloatType;
  cubeTexture.needsUpdate = true;

  return cubeTexture;
}

function createImageFromFloatData(data, width, height) {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  const imageData = ctx.createImageData(width, height);

  // Convert Float RGB to Uint8 RGBA
  for (let i = 0, j = 0; i < data.length; i += 3, j += 4) {
    imageData.data[j] = Math.min(255, data[i] * 255);     // R
    imageData.data[j + 1] = Math.min(255, data[i + 1] * 255); // G
    imageData.data[j + 2] = Math.min(255, data[i + 2] * 255); // B
    imageData.data[j + 3] = 255;     // A
  }

  ctx.putImageData(imageData, 0, 0);
  return canvas;
}

let hdrEnvmap = null;
let whiteEnvmap = null;

function updateEnvironment() {
  const envmap = ui_state['useWhiteEnvmap'] ? whiteEnvmap : hdrEnvmap;
  scene.background = envmap;
  scene.environment = envmap;
  ui_state['mtl'].envMap = envmap;

  // Update all materials in the scene
  scene.traverse((object) => {
    if (object.material && object.material.envMap !== undefined) {
      object.material.envMap = envmap;
      object.material.needsUpdate = true;
    }
  });
}

async function initScene() {

  const envmap = await new HDRCubeTextureLoader()
    .setPath( 'assets/textures/cube/pisaHDR/' )
    .loadAsync( [ 'px.hdr', 'nx.hdr', 'py.hdr', 'ny.hdr', 'pz.hdr', 'nz.hdr' ] )

  console.log(envmap);
  hdrEnvmap = envmap;
  whiteEnvmap = createWhiteEnvmap();

  // Set initial environment
  updateEnvironment();

  const camera = new THREE.PerspectiveCamera(75, window.innerWidth / window.innerHeight, 0.1, 1000);
  camera.position.z = ui_state['camera_z'];

  const renderer = new THREE.WebGLRenderer();
  renderer.setSize(window.innerWidth, window.innerHeight);
  document.body.appendChild(renderer.domElement);

  const rootNodes = await loadScenes();

  for (const rootNode of rootNodes) {
    scene.add(rootNode);
  }

  function animate() {

    for (const rootNode of rootNodes) {
      rootNode.rotation.y += 0.01 * ui_state['rot_scale'];
      //rootNode.rotation.x += 0.02 * ui_state['rot_scale'];
    }

    camera.position.z = ui_state['camera_z'];

  if (ui_state['material_changed']) {
      ui_state['material_changed'] = false;

      if (ui_state['shader_normal']) {
        //mesh0.material = normalMat;

      } else {
        //mesh0.material = pbrMaterial;
      }

      scene.traverse((object) => {
        if (object.material && object.material.envMap !== undefined) {

          console.log("material changed", object.material);

          object.material.needsUpdate = true;

          object.material.color.r = ui_state['diffuse'].r / 255.0;
          object.material.color.g = ui_state['diffuse'].g / 255.0;
          object.material.color.b = ui_state['diffuse'].b / 255.0;
          console.log("diffuse", ui_state['diffuse']);
          console.log("mat_diffuse", object.material.color);

          // Toggle diffuse texture on/off
          if (ui_state['diffuse_texture_enabled'] && ui_state['diffuseMap']) {
            // Keep original diffuse texture if it exists
            object.material.map = ui_state['diffuseMap'];
          } else {
            console.log("Disabling diffuse texture");
            // Disable diffuse texture
            if (object.material.map) {
              ui_state['diffuseMap'] = object.material.map; // Store original diffuse texture
            }
            object.material.map = null;
          }

          // Toggle normal texture on/off
          if (ui_state['normal_texture_enabled'] && ui_state['normalMap']) {
            // Keep original normal texture if it exists
            object.material.normalMap = ui_state['normalMap'];
          } else {
            console.log("Disabling normal texture");
            // Disable normal texture
            if (object.material.normalMap) {
              ui_state['normalMap'] = object.material.normalMap; // Store original normal texture
            }
            object.material.normalMap = null;
          }

          // Toggle AO texture on/off
          if (ui_state['ao_texture_enabled'] && ui_state['aoMap']) {
            // Keep original AO texture if it exists
            object.material.aoMap = ui_state['aoMap'];
          } else {
            console.log("Disabling AO texture");
            // Disable AO texture
            if (object.material.aoMap) {
              ui_state['aoMap'] = object.material.aoMap; // Store original AO texture
            }
            object.material.aoMap = null;
          }

          // Toggle alpha texture on/off
          if (ui_state['alpha_texture_enabled'] && ui_state['alphaMap'] ) {
            // Keep original alpha texture if it exists
            object.material.alphaMap = ui_state['alphaMap'];
          } else {
            // Disable alpha texture
            if (object.material.alphaMap) {
              ui_state['alphaMap'] = object.material.alphaMap; // Store original alpha texture
            }
            object.material.alphaMap = null;
          }

          object.material.emissive.r = ui_state['emissive'].r / 255.0;
          object.material.emissive.g = ui_state['emissive'].g / 255.0;
          object.material.emissive.b = ui_state['emissive'].b / 255.0;

          object.material.roughness = ui_state['roughness'];
          object.material.metalness = ui_state['metalness'];
          object.material.ior = ui_state['ior'];
          object.material.clearcoat = ui_state['clearcoat'];
          object.material.clearcoatRoughness = ui_state['clearcoatRoughness'];
          object.material.specularIntensity = ui_state['specularIntensity'];
          object.material.opacity = ui_state['opacity'];
          object.material.transparent = ui_state['transparent'] || (ui_state['opacity'] < 1.0);
        }
      });
    }



    renderer.render(scene, camera);

  }

  renderer.setAnimationLoop(animate);
}

initScene();
