import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';
import {
  createConfiguredLightUSDLoader,
  parseUSDSceneFromArrayBuffer
} from 'lightusd/LoaderConfigUtils.js';

const configuredMujocoDir = import.meta.env.VITE_MUJOCO_WASM_DIR || '';
const MUJOCO_DIST = configuredMujocoDir ? `/@fs${configuredMujocoDir}` : './assets/mujoco';
const MUJOCO_JS = `${MUJOCO_DIST}/mujoco_physics.js`;
const MUJOCO_WASM = `${MUJOCO_DIST}/mujoco_physics.wasm`;
const USDA_ASSET_URL = './assets/physics-robot-arm.usda';

const state = {
  lightusdLoader: null,
  lightusdNative: null,
  mujoco: null,
  model: null,
  data: null,
  usdObject: null,
  physicsJson: null,
  paused: false,
  driveMode: 'servo',
  lastTime: performance.now(),
  targets: {
    shoulder: THREE.MathUtils.degToRad(34),
    elbow: THREE.MathUtils.degToRad(-52),
  },
  speed: 1,
  sim: {
    shoulder: null,
    upper: null,
    elbow: null,
    lower: null,
    gripper: null,
  },
};

const els = {
  status: document.getElementById('status'),
  playPause: document.getElementById('playPause'),
  reset: document.getElementById('reset'),
  fit: document.getElementById('fit'),
  driveMode: document.getElementById('driveMode'),
  shoulderRange: document.getElementById('shoulderTarget'),
  shoulderNumber: document.getElementById('shoulderNumber'),
  elbowRange: document.getElementById('elbowTarget'),
  elbowNumber: document.getElementById('elbowNumber'),
  speedRange: document.getElementById('speed'),
  speedNumber: document.getElementById('speedNumber'),
  simTime: document.getElementById('simTime'),
  modelStats: document.getElementById('modelStats'),
  usdStats: document.getElementById('usdStats'),
  physicsJson: document.getElementById('physicsJson'),
  usdaPreview: document.getElementById('usdaPreview'),
};

function setStatus(text) {
  els.status.textContent = text;
}

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x15181c);

const camera = new THREE.PerspectiveCamera(46, window.innerWidth / window.innerHeight, 0.01, 100);
camera.up.set(0, 0, 1);
camera.position.set(2.6, -3.0, 2.1);

const renderer = new THREE.WebGLRenderer({ antialias: true, preserveDrawingBuffer: true });
renderer.setPixelRatio(window.devicePixelRatio);
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFShadowMap;
document.body.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.target.set(0.65, 0, 0.55);
controls.update();

const world = new THREE.Group();
const restRoot = new THREE.Group();
const simRoot = new THREE.Group();
scene.add(world);
world.add(restRoot);
world.add(simRoot);

scene.add(new THREE.HemisphereLight(0xdde8f6, 0x24272c, 1.5));
const keyLight = new THREE.DirectionalLight(0xffffff, 2.3);
keyLight.position.set(2.6, -3.6, 4.2);
keyLight.castShadow = true;
scene.add(keyLight);
scene.add(new THREE.GridHelper(8, 32, 0x52606c, 0x2b3138).rotateX(Math.PI / 2));
scene.add(new THREE.AxesHelper(0.42));

const matBase = new THREE.MeshStandardMaterial({ color: 0x6b7280, roughness: 0.62, metalness: 0.02 });
const matUpper = new THREE.MeshStandardMaterial({ color: 0x38bdf8, roughness: 0.48, metalness: 0.03 });
const matLower = new THREE.MeshStandardMaterial({ color: 0x5eead4, roughness: 0.5, metalness: 0.02 });
const matJoint = new THREE.MeshStandardMaterial({ color: 0xe5e7eb, roughness: 0.42, metalness: 0.04 });
const matTarget = new THREE.MeshStandardMaterial({ color: 0xf59e0b, roughness: 0.55, metalness: 0.02 });

function addBox(parent, name, size, position, material) {
  const mesh = new THREE.Mesh(new THREE.BoxGeometry(size[0], size[1], size[2]), material);
  mesh.name = name;
  mesh.position.fromArray(position);
  mesh.castShadow = true;
  mesh.receiveShadow = true;
  parent.add(mesh);
  return mesh;
}

function addJointMarker(parent, name, radius = 0.09) {
  const mesh = new THREE.Mesh(new THREE.CylinderGeometry(radius, radius, 0.07, 32), matJoint);
  mesh.name = name;
  mesh.rotation.x = Math.PI / 2;
  mesh.castShadow = true;
  parent.add(mesh);
  return mesh;
}

function buildSimulationDisplay() {
  addBox(simRoot, 'sim_base', [0.62, 0.62, 0.28], [0, 0, 0.14], matBase);
  const shoulder = new THREE.Group();
  shoulder.name = 'sim_shoulder_pivot';
  shoulder.position.set(0, 0, 0.52);
  simRoot.add(shoulder);
  addJointMarker(shoulder, 'sim_shoulder_joint');
  const upper = addBox(shoulder, 'sim_upper_arm', [0.9, 0.16, 0.16], [0.45, 0, 0], matUpper);
  const elbow = new THREE.Group();
  elbow.name = 'sim_elbow_pivot';
  elbow.position.set(0.9, 0, 0);
  shoulder.add(elbow);
  addJointMarker(elbow, 'sim_elbow_joint', 0.075);
  const lower = addBox(elbow, 'sim_forearm', [0.7, 0.12, 0.12], [0.35, 0, 0], matLower);
  const gripper = addBox(elbow, 'sim_gripper', [0.13, 0.28, 0.1], [0.77, 0, 0], matTarget);
  state.sim = { shoulder, upper, elbow, lower, gripper };
}

function setRestObjectStyle(root) {
  root.traverse((obj) => {
    if (obj.isMesh && obj.material) {
      const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
      obj.material = mats.map((mat) => {
        const copy = mat.clone();
        copy.color?.set?.(0xf59e0b);
        copy.transparent = true;
        copy.opacity = 0.22;
        copy.depthWrite = false;
        copy.wireframe = true;
        return copy;
      });
    }
  });
}

function disposeObject(root) {
  root.traverse((obj) => {
    obj.geometry?.dispose?.();
    if (obj.material) {
      const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
      for (const mat of mats) mat.dispose?.();
    }
  });
}

function fitCamera() {
  const box = new THREE.Box3().setFromObject(simRoot);
  if (state.usdObject) box.union(new THREE.Box3().setFromObject(state.usdObject));
  if (box.isEmpty()) return;
  const sphere = box.getBoundingSphere(new THREE.Sphere());
  controls.target.copy(sphere.center);
  const distance = Math.max(2.5, sphere.radius * 3.2);
  camera.position.copy(sphere.center).add(new THREE.Vector3(1.7, -2.2, 1.35).normalize().multiplyScalar(distance));
  camera.near = Math.max(0.001, distance / 100);
  camera.far = distance * 100;
  camera.updateProjectionMatrix();
  controls.update();
}

async function usdaText() {
  const response = await fetch(USDA_ASSET_URL);
  if (!response.ok) {
    throw new Error(`Failed to load ${USDA_ASSET_URL}: HTTP ${response.status}`);
  }
  return response.text();
}

async function loadLightUSDScene() {
  setStatus('Loading LightUSD WASM...');
  // Still on the legacy product: physics extraction reads LightUSDLoaderNative's
  // physics JSON contract. Pinned while next is the default.
  state.lightusdLoader = await createConfiguredLightUSDLoader({ backend: 'legacy' });
  LightUSDLoaderUtils.setLightUSD(state.lightusdLoader.native_);
  state.lightusdNative = new state.lightusdLoader.native_.LightUSDLoaderNative();

  const text = await usdaText();
  const bytes = new TextEncoder().encode(text);
  const arrayBuffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  const sceneData = await parseUSDSceneFromArrayBuffer(state.lightusdLoader, arrayBuffer, 'embedded_robot.usda');
  const rootNode = sceneData.getDefaultRootNode();
  const defaultMaterial = LightUSDLoaderUtils.createDefaultMaterial();
  state.usdObject = await LightUSDLoaderUtils.buildThreeNode(rootNode, defaultMaterial, sceneData, {
    overrideMaterial: false,
  });
  state.usdObject.name = 'USD rest pose';
  setRestObjectStyle(state.usdObject);
  restRoot.add(state.usdObject);

  if (!state.lightusdNative.loadFromBinary(bytes, 'embedded_robot.usda')) {
    throw new Error(state.lightusdNative.error() || 'LightUSD physics extraction failed.');
  }
  const jsonText = state.lightusdNative.extractPhysicsSceneJSON();
  if (!jsonText) {
    throw new Error(state.lightusdNative.error() || 'LightUSD returned empty physics JSON.');
  }
  state.physicsJson = JSON.parse(jsonText);
  els.physicsJson.textContent = JSON.stringify(state.physicsJson, null, 2);
  els.usdaPreview.textContent = text;
  els.usdStats.textContent = `${state.physicsJson.prims?.length ?? 0} prims`;
}

async function loadMuJoCoPhysics() {
  setStatus('Loading MuJoCo physics WASM...');
  const module = await import(/* @vite-ignore */ MUJOCO_JS);
  state.mujoco = await module.default({
    locateFile: (path) => (path.endsWith('.wasm') ? MUJOCO_WASM : `${MUJOCO_DIST}/${path}`),
  });
}

function addMuJoCoBox(mj, body, name, halfSize, pos, rgba, mass = 0) {
  const geom = mj.MjsGeom.add(body, name);
  mj.MjsGeom.setType(geom, mj.GEOM_BOX);
  mj.MjsGeom.setSize(geom, halfSize[0], halfSize[1], halfSize[2]);
  mj.MjsGeom.setPos(geom, pos[0], pos[1], pos[2]);
  mj.MjsGeom.setRGBA(geom, rgba[0], rgba[1], rgba[2], rgba[3]);
  if (mass > 0) mj.MjsGeom.setMass(geom, mass);
  return geom;
}

function buildMuJoCoModel() {
  const mj = state.mujoco;
  const spec = new mj.MjSpec();
  spec.setModelName('LightUSDEmbeddedArm');
  spec.setTimestep(0.005);
  spec.setGravity(0, 0, -9.80665);
  const worldBody = spec.worldBody();

  const plane = mj.MjsGeom.add(worldBody, 'ground');
  mj.MjsGeom.setType(plane, mj.GEOM_PLANE);
  mj.MjsGeom.setSize(plane, 5, 5, 0.02);
  mj.MjsGeom.setRGBA(plane, 0.16, 0.18, 0.2, 1);
  mj.MjsGeom.setFriction(plane, 1.0, 0.01, 0.001);

  const light = mj.MjsLight.add(worldBody, 'key');
  mj.MjsLight.setPos(light, 2.5, -3.0, 4.0);
  mj.MjsLight.setDir(light, -0.5, 0.65, -1.0);
  mj.MjsLight.setDiffuse(light, 0.8, 0.8, 0.8);

  const base = mj.MjsBody.add(worldBody, 'Base');
  mj.MjsBody.setPos(base, 0, 0, 0);
  addMuJoCoBox(mj, base, 'BaseBlock', [0.31, 0.31, 0.14], [0, 0, 0.14], [0.42, 0.45, 0.5, 1], 8);

  const shoulder = mj.MjsBody.add(base, 'Shoulder');
  mj.MjsBody.setPos(shoulder, 0, 0, 0.52);
  const shoulderJoint = mj.MjsJoint.add(shoulder, 'ShoulderJoint');
  mj.MjsJoint.setType(shoulderJoint, mj.JNT_HINGE);
  mj.MjsJoint.setAxis(shoulderJoint, 0, 1, 0);
  mj.MjsJoint.setRange(shoulderJoint, THREE.MathUtils.degToRad(-130), THREE.MathUtils.degToRad(130));
  mj.MjsJoint.setDamping(shoulderJoint, 5.0);
  addMuJoCoBox(mj, shoulder, 'UpperArm', [0.45, 0.08, 0.08], [0.45, 0, 0], [0.22, 0.74, 0.97, 1], 2.2);

  const elbow = mj.MjsBody.add(shoulder, 'Elbow');
  mj.MjsBody.setPos(elbow, 0.9, 0, 0);
  const elbowJoint = mj.MjsJoint.add(elbow, 'ElbowJoint');
  mj.MjsJoint.setType(elbowJoint, mj.JNT_HINGE);
  mj.MjsJoint.setAxis(elbowJoint, 0, 1, 0);
  mj.MjsJoint.setRange(elbowJoint, THREE.MathUtils.degToRad(-135), THREE.MathUtils.degToRad(135));
  mj.MjsJoint.setDamping(elbowJoint, 3.0);
  addMuJoCoBox(mj, elbow, 'Forearm', [0.35, 0.06, 0.06], [0.35, 0, 0], [0.37, 0.92, 0.83, 1], 1.4);
  addMuJoCoBox(mj, elbow, 'Gripper', [0.065, 0.14, 0.05], [0.77, 0, 0], [0.96, 0.62, 0.04, 1], 0.25);

  state.model = spec.compile();
  state.data = new mj.PhysicsData(state.model);
  spec.delete();
  resetSimulation();
  els.modelStats.textContent =
    `${state.model.nbody()} bodies, ${state.model.njnt()} joints, ${state.model.nq()} qpos`;
}

function resetSimulation() {
  if (!state.model || !state.data) return;
  const mj = state.mujoco;
  mj.mj_resetData(state.model, state.data);
  const qpos = state.data.qpos();
  const qvel = state.data.qvel();
  qpos[0] = state.targets.shoulder;
  qpos[1] = state.targets.elbow;
  qvel.fill(0);
  mj.mj_forward(state.model, state.data);
  updateDisplayFromSimulation();
}

function applyServoForces(dt) {
  if (state.driveMode !== 'servo') return;
  const qpos = state.data.qpos();
  const qvel = state.data.qvel();
  const gains = [
    { target: state.targets.shoulder, kp: 58, kd: 13 },
    { target: state.targets.elbow, kp: 72, kd: 11 },
  ];
  for (let i = 0; i < gains.length; i++) {
    const error = gains[i].target - qpos[i];
    qvel[i] += (error * gains[i].kp - qvel[i] * gains[i].kd) * dt;
  }
}

function stepSimulation(deltaSeconds) {
  if (!state.model || !state.data || state.paused) return;
  const mj = state.mujoco;
  const baseDt = state.model.timestep();
  const scaled = Math.min(0.05, deltaSeconds * state.speed);
  const steps = Math.max(1, Math.min(12, Math.ceil(scaled / baseDt)));
  for (let i = 0; i < steps; i++) {
    applyServoForces(baseDt);
    mj.mj_step(state.model, state.data);
  }
  updateDisplayFromSimulation();
}

function updateDisplayFromSimulation() {
  if (!state.data || !state.sim.shoulder) return;
  const qpos = state.data.qpos();
  state.sim.shoulder.rotation.y = qpos[0] || 0;
  state.sim.elbow.rotation.y = qpos[1] || 0;
  els.simTime.textContent = `${state.data.time().toFixed(3)} s`;
}

function bindRangeAndNumber(range, number, onValue) {
  const sync = (value) => {
    range.value = String(value);
    number.value = String(value);
    onValue(Number(value));
  };
  range.addEventListener('input', () => sync(range.value));
  number.addEventListener('input', () => sync(number.value));
}

function bindUI() {
  els.playPause.addEventListener('click', () => {
    state.paused = !state.paused;
    els.playPause.textContent = state.paused ? 'Play' : 'Pause';
    setStatus(state.paused ? 'Paused.' : 'Simulating.');
  });
  els.reset.addEventListener('click', () => {
    resetSimulation();
    setStatus('Simulation reset.');
  });
  els.fit.addEventListener('click', fitCamera);
  els.driveMode.addEventListener('change', () => {
    state.driveMode = els.driveMode.value;
    setStatus(state.driveMode === 'servo' ? 'Servo drive enabled.' : 'Passive joint mode.');
  });
  bindRangeAndNumber(els.shoulderRange, els.shoulderNumber, (degrees) => {
    state.targets.shoulder = THREE.MathUtils.degToRad(degrees);
  });
  bindRangeAndNumber(els.elbowRange, els.elbowNumber, (degrees) => {
    state.targets.elbow = THREE.MathUtils.degToRad(degrees);
  });
  bindRangeAndNumber(els.speedRange, els.speedNumber, (value) => {
    state.speed = value;
  });
}

function onResize() {
  camera.aspect = window.innerWidth / window.innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(window.innerWidth, window.innerHeight);
}

function animate(now) {
  const delta = Math.min(0.05, (now - state.lastTime) / 1000);
  state.lastTime = now;
  stepSimulation(delta);
  controls.update();
  renderer.render(scene, camera);
  requestAnimationFrame(animate);
}

async function main() {
  bindUI();
  buildSimulationDisplay();
  await loadLightUSDScene();
  await loadMuJoCoPhysics();
  buildMuJoCoModel();
  fitCamera();
  setStatus('Simulating.');
  requestAnimationFrame(animate);
}

window.addEventListener('resize', onResize);
main().catch((err) => {
  console.error(err);
  setStatus(err.message || String(err));
  els.playPause.disabled = true;
  els.reset.disabled = true;
});
