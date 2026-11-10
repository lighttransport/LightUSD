#!/usr/bin/env node
// Batch USD Physics + MuJoCo physics-only simulation smoke test.

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import LightUSDFactory from '../src/lightusd/lightusd.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const DEFAULT_USDA = path.resolve(__dirname, '../assets/physics-robot-arm.usda');
const DEFAULT_MUJOCO_DIST = path.resolve(process.env.LIGHTUSD_VERIFY_CACHE ||
  path.join(__dirname, '../../../.cache/lightusd-verification'), 'mujoco/wasm/dist');
const MUJOCO_DIST = path.resolve(process.env.MUJOCO_WASM_DIR || DEFAULT_MUJOCO_DIST);
const MUJOCO_JS = path.resolve(process.env.MUJOCO_PHYSICS_JS ||
  path.join(MUJOCO_DIST, 'mujoco_physics.js'));
const MUJOCO_WASM = path.resolve(process.env.MUJOCO_PHYSICS_WASM ||
  path.join(MUJOCO_DIST, 'mujoco_physics.wasm'));
const DEG_TO_RAD = Math.PI / 180;
const RAD_TO_DEG = 180 / Math.PI;

function printHelp() {
  console.log(`
LightUSD USD Physics + MuJoCo physics-only batch simulator

Usage:
  node cli/phys-sim.js [options]

Options:
  --usda <path>          USDA scene to load (default: assets/physics-robot-arm.usda)
  --seconds <n>          Simulation duration in seconds (default: 3)
  --target <a,b>         Shoulder/elbow hold target in degrees (default: 34,-52)
  --mode <motor|passive> Motor hold or passive gravity sim (default: motor)
  --tolerance <deg>      Max allowed final joint error in motor mode (default: 2)
  --dump-trajectory <p>  Write per-step trajectory JSON to path, or "-" for stdout
  --trajectory-stride <n>
                         Record every n simulation steps (default: 1)
  --dump-physics-json    Print LightUSD extracted physics JSON
  --require-newton       Require extracted Newton API schemas
  --json                 Print machine-readable result JSON
  Environment: MUJOCO_WASM_DIR, MUJOCO_PHYSICS_JS, MUJOCO_PHYSICS_WASM
  -h, --help             Show this help
`);
}

function parseArgs(argv = process.argv.slice(2)) {
  const opts = {
    usdaPath: DEFAULT_USDA,
    seconds: 3,
    target: [34 * DEG_TO_RAD, -52 * DEG_TO_RAD],
    mode: 'motor',
    tolerance: 2 * DEG_TO_RAD,
    trajectoryPath: null,
    trajectoryStride: 1,
    dumpPhysicsJson: false,
    requireNewton: false,
    json: false,
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      printHelp();
      process.exit(0);
    } else if (arg === '--usda') {
      opts.usdaPath = path.resolve(requireValue(argv, ++i, arg));
    } else if (arg === '--seconds') {
      opts.seconds = positiveNumber(requireValue(argv, ++i, arg), arg);
    } else if (arg === '--target') {
      opts.target = parseTarget(requireValue(argv, ++i, arg));
    } else if (arg === '--mode') {
      opts.mode = requireValue(argv, ++i, arg);
      if (!['motor', 'passive'].includes(opts.mode)) {
        throw new Error('--mode must be motor or passive');
      }
    } else if (arg === '--tolerance') {
      opts.tolerance = positiveNumber(requireValue(argv, ++i, arg), arg) * DEG_TO_RAD;
    } else if (arg === '--dump-trajectory') {
      const value = requireValue(argv, ++i, arg, true);
      opts.trajectoryPath = value === '-' ? '-' : path.resolve(value);
    } else if (arg === '--trajectory-stride') {
      opts.trajectoryStride = positiveInteger(requireValue(argv, ++i, arg), arg);
    } else if (arg === '--dump-physics-json') {
      opts.dumpPhysicsJson = true;
    } else if (arg === '--require-newton') {
      opts.requireNewton = true;
    } else if (arg === '--json') {
      opts.json = true;
    } else {
      throw new Error(`Unknown option: ${arg}`);
    }
  }

  return opts;
}

function requireValue(argv, index, optionName, allowDash = false) {
  const value = argv[index];
  if (!value || (!allowDash && value.startsWith('-'))) {
    throw new Error(`${optionName} requires a value`);
  }
  return value;
}

function positiveNumber(value, optionName) {
  const n = Number(value);
  if (!Number.isFinite(n) || n <= 0) {
    throw new Error(`${optionName} requires a positive number`);
  }
  return n;
}

function positiveInteger(value, optionName) {
  const n = Number(value);
  if (!Number.isInteger(n) || n <= 0) {
    throw new Error(`${optionName} requires a positive integer`);
  }
  return n;
}

function parseTarget(text) {
  const parts = text.split(',').map((s) => Number(s.trim()));
  if (parts.length !== 2 || parts.some((n) => !Number.isFinite(n))) {
    throw new Error('--target must be two comma-separated degree values');
  }
  return [parts[0] * DEG_TO_RAD, parts[1] * DEG_TO_RAD];
}

async function loadLightUSDPhysics(usdaPath) {
  const text = fs.readFileSync(usdaPath, 'utf8');
  const bytes = new TextEncoder().encode(text);
  const lightusd = await LightUSDFactory();
  const native = new lightusd.NextUSDZConverterNative();
  try {
    if (!native.loadFromBinary(bytes, path.basename(usdaPath))) {
      throw new Error(native.error() || 'LightUSD loadFromBinary failed');
    }
    const jsonText = native.extractPhysicsSceneJSON();
    if (!jsonText) {
      throw new Error(native.error() || 'LightUSD extractPhysicsSceneJSON returned empty JSON');
    }
    return {
      text,
      physics: JSON.parse(jsonText),
      warn: native.warn?.() || '',
    };
  } finally {
    native.delete();
  }
}

async function loadMuJoCoPhysics() {
  if (!fs.existsSync(MUJOCO_JS) || !fs.existsSync(MUJOCO_WASM)) {
    throw new Error(`MuJoCo physics-only artifacts not found in ${MUJOCO_DIST}`);
  }
  const module = await import(pathToFileURL(MUJOCO_JS).href);
  return module.default({
    locateFile: (filename) => (filename.endsWith('.wasm') ? MUJOCO_WASM : path.join(MUJOCO_DIST, filename)),
  });
}

function addBox(mj, body, name, halfSize, pos, rgba, mass = 0) {
  const geom = mj.MjsGeom.add(body, name);
  mj.MjsGeom.setType(geom, mj.GEOM_BOX);
  mj.MjsGeom.setSize(geom, halfSize[0], halfSize[1], halfSize[2]);
  mj.MjsGeom.setPos(geom, pos[0], pos[1], pos[2]);
  mj.MjsGeom.setRGBA(geom, rgba[0], rgba[1], rgba[2], rgba[3]);
  if (mass > 0) mj.MjsGeom.setMass(geom, mass);
  return geom;
}

function buildModel(mj) {
  const spec = new mj.MjSpec();
  spec.setModelName('LightUSDBatchArm');
  spec.setTimestep(0.005);
  spec.setGravity(0, 0, -9.80665);
  const worldBody = spec.worldBody();

  const ground = mj.MjsGeom.add(worldBody, 'virtual_ground');
  mj.MjsGeom.setType(ground, mj.GEOM_PLANE);
  mj.MjsGeom.setSize(ground, 5, 5, 0.02);
  mj.MjsGeom.setRGBA(ground, 0.18, 0.2, 0.22, 1);
  mj.MjsGeom.setFriction(ground, 1.0, 0.01, 0.001);

  const base = mj.MjsBody.add(worldBody, 'Base');
  mj.MjsBody.setPos(base, 0, 0, 0);
  addBox(mj, base, 'BaseBlock', [0.31, 0.31, 0.14], [0, 0, 0.14], [0.42, 0.45, 0.5, 1], 8);

  const shoulder = mj.MjsBody.add(base, 'Shoulder');
  mj.MjsBody.setPos(shoulder, 0, 0, 0.52);
  const shoulderJoint = mj.MjsJoint.add(shoulder, 'ShoulderJoint');
  mj.MjsJoint.setType(shoulderJoint, mj.JNT_HINGE);
  mj.MjsJoint.setAxis(shoulderJoint, 0, 1, 0);
  mj.MjsJoint.setRange(shoulderJoint, -130 * DEG_TO_RAD, 130 * DEG_TO_RAD);
  mj.MjsJoint.setDamping(shoulderJoint, 0.2);
  addBox(mj, shoulder, 'UpperArm', [0.45, 0.08, 0.08], [0.45, 0, 0], [0.22, 0.74, 0.97, 1], 2.2);

  const elbow = mj.MjsBody.add(shoulder, 'Elbow');
  mj.MjsBody.setPos(elbow, 0.9, 0, 0);
  const elbowJoint = mj.MjsJoint.add(elbow, 'ElbowJoint');
  mj.MjsJoint.setType(elbowJoint, mj.JNT_HINGE);
  mj.MjsJoint.setAxis(elbowJoint, 0, 1, 0);
  mj.MjsJoint.setRange(elbowJoint, -135 * DEG_TO_RAD, 135 * DEG_TO_RAD);
  mj.MjsJoint.setDamping(elbowJoint, 0.15);
  addBox(mj, elbow, 'Forearm', [0.35, 0.06, 0.06], [0.35, 0, 0], [0.37, 0.92, 0.83, 1], 1.4);
  addBox(mj, elbow, 'Picker', [0.065, 0.14, 0.05], [0.77, 0, 0], [0.96, 0.62, 0.04, 1], 0.25);

  const model = spec.compile();
  spec.delete();
  return model;
}

function clamp(value, low, high) {
  return Math.max(low, Math.min(high, value));
}

function motorTorqueProxy(model, data, target, dt) {
  const qpos = data.qpos();
  const qvel = data.qvel();
  const kp = [220, 150];
  const kd = [18, 12];
  const maxDelta = [0.08, 0.08];
  let peak = 0;
  const torques = [0, 0];

  for (let i = 0; i < 2; i++) {
    const err = target[i] - qpos[i];
    const torque = kp[i] * err - kd[i] * qvel[i];
    torques[i] = torque;
    const delta = clamp(err * 0.65, -maxDelta[i], maxDelta[i]);
    qpos[i] += delta;
    qvel[i] = delta / dt;
    peak = Math.max(peak, Math.abs(torque));
  }

  return { peak, torques };
}

function pickerPosition(q) {
  const baseZ = 0.52;
  const l1 = 0.9;
  const l2 = 0.77;
  const q1 = q[0];
  const q2 = q[0] + q[1];
  return [
    l1 * Math.cos(q1) + l2 * Math.cos(q2),
    0,
    baseZ - l1 * Math.sin(q1) - l2 * Math.sin(q2),
  ];
}

function hasApi(prim, apiName) {
  return (prim.apiSchemas || []).some((api) => api === apiName || api.startsWith(`${apiName}:`));
}

function validatePhysicsJSON(physics, opts = {}) {
  const prims = Array.isArray(physics.prims) ? physics.prims : [];
  const joints = prims.filter((prim) => prim.type === 'PhysicsRevoluteJoint');
  const scene = prims.find((prim) => prim.type === 'PhysicsScene');
  if (!scene) throw new Error('USDA did not extract a PhysicsScene');
  if (joints.length < 2) throw new Error(`Expected at least 2 PhysicsRevoluteJoint prims, got ${joints.length}`);
  const newtonApis = prims.flatMap((prim) => prim.apiSchemas || [])
    .filter((api) => api.startsWith('Newton'));
  if (opts.requireNewton && !hasApi(scene, 'NewtonSceneAPI')) {
    throw new Error('USDA did not extract NewtonSceneAPI');
  }
  return { prims, joints, scene, newtonApis };
}

function trajectorySample(step, time, data, target, motorTorque) {
  const qpos = Array.from(data.qpos()).slice(0, 2);
  const qvel = Array.from(data.qvel()).slice(0, 2);
  const jointError = qpos.map((v, i) => target[i] - v);
  return {
    step,
    time,
    qpos,
    qposDeg: qpos.map((v) => v * RAD_TO_DEG),
    qvel,
    target: [...target],
    targetDeg: target.map((v) => v * RAD_TO_DEG),
    jointError,
    jointErrorDeg: jointError.map((v) => v * RAD_TO_DEG),
    motorTorque,
    pickerPosition: pickerPosition(qpos),
  };
}

async function runSimulation(opts) {
  const loaded = await loadLightUSDPhysics(opts.usdaPath);
  const extracted = validatePhysicsJSON(loaded.physics, opts);
  const mj = await loadMuJoCoPhysics();
  const model = buildModel(mj);
  const data = new mj.PhysicsData(model);
  const dt = model.timestep();
  const steps = Math.max(1, Math.round(opts.seconds / dt));
  let peakTorque = 0;
  let peakError = 0;
  let minPickerZ = Number.POSITIVE_INFINITY;
  const trajectory = opts.trajectoryPath ? [] : null;

  try {
    const qpos = data.qpos();
    const qvel = data.qvel();
    qpos[0] = opts.target[0];
    qpos[1] = opts.target[1];
    qvel.fill(0);
    mj.mj_forward(model, data);
    if (trajectory) {
      trajectory.push(trajectorySample(0, data.time(), data, opts.target, [0, 0]));
    }

    for (let i = 0; i < steps; i++) {
      let motorTorque = [0, 0];
      if (opts.mode === 'motor') {
        const motor = motorTorqueProxy(model, data, opts.target, dt);
        motorTorque = motor.torques;
        peakTorque = Math.max(peakTorque, motor.peak);
      }
      mj.mj_step(model, data);
      if (opts.mode === 'motor') {
        const motor = motorTorqueProxy(model, data, opts.target, dt);
        motorTorque = motor.torques;
        peakTorque = Math.max(peakTorque, motor.peak);
        mj.mj_forward(model, data);
      }
      const q = data.qpos();
      peakError = Math.max(
        peakError,
        Math.abs(q[0] - opts.target[0]),
        Math.abs(q[1] - opts.target[1])
      );
      minPickerZ = Math.min(minPickerZ, pickerPosition(q)[2]);
      if (trajectory && ((i + 1) % opts.trajectoryStride === 0 || i + 1 === steps)) {
        trajectory.push(trajectorySample(i + 1, data.time(), data, opts.target, motorTorque));
      }
    }

    const finalQ = Array.from(data.qpos()).slice(0, 2);
    const finalError = Math.max(
      Math.abs(finalQ[0] - opts.target[0]),
      Math.abs(finalQ[1] - opts.target[1])
    );
    const picker = pickerPosition(finalQ);
    const passed = opts.mode === 'passive' || (finalError <= opts.tolerance && picker[2] > 0.02);

    return {
      passed,
      mode: opts.mode,
      usdaPath: opts.usdaPath,
      primCount: extracted.prims.length,
      jointCount: extracted.joints.length,
      newtonApiCount: extracted.newtonApis.length,
      model: {
        nbody: model.nbody(),
        njnt: model.njnt(),
        nq: model.nq(),
        timestep: dt,
      },
      seconds: steps * dt,
      steps,
      targetDeg: opts.target.map((v) => v * RAD_TO_DEG),
      finalQDeg: finalQ.map((v) => v * RAD_TO_DEG),
      finalErrorDeg: finalError * RAD_TO_DEG,
      peakErrorDeg: peakError * RAD_TO_DEG,
      peakMotorTorque: peakTorque,
      pickerPosition: picker,
      minPickerZ,
      warn: loaded.warn.trim(),
      physics: loaded.physics,
      trajectory,
    };
  } finally {
    data.delete();
    model.delete();
  }
}

function writeTrajectory(result, opts) {
  if (!opts.trajectoryPath) return;
  const payload = {
    metadata: {
      usdaPath: result.usdaPath,
      mode: result.mode,
      seconds: result.seconds,
      steps: result.steps,
      timestep: result.model.timestep,
      stride: opts.trajectoryStride,
      jointNames: ['shoulder', 'elbow'],
      targetDeg: result.targetDeg,
      passed: result.passed,
      finalErrorDeg: result.finalErrorDeg,
      minPickerZ: result.minPickerZ,
      peakMotorTorque: result.peakMotorTorque,
    },
    samples: result.trajectory || [],
  };
  const text = `${JSON.stringify(payload, null, 2)}\n`;
  if (opts.trajectoryPath === '-') {
    console.log(text.trimEnd());
    return;
  }
  fs.mkdirSync(path.dirname(opts.trajectoryPath), { recursive: true });
  fs.writeFileSync(opts.trajectoryPath, text, 'utf8');
}

function printResult(result, opts) {
  if (opts.dumpPhysicsJson) {
    console.log(JSON.stringify(result.physics, null, 2));
  }
  if (opts.json) {
    const { physics, trajectory, ...summary } = result;
    if (opts.trajectoryPath && trajectory) {
      summary.trajectory = {
        path: opts.trajectoryPath,
        samples: trajectory.length,
        stride: opts.trajectoryStride,
      };
    }
    console.log(JSON.stringify(summary, null, 2));
    return;
  }

  console.log(`Loaded ${path.relative(process.cwd(), result.usdaPath)}: ${result.primCount} prims, ${result.jointCount} revolute joints`);
  if (result.newtonApiCount) {
    console.log(`Newton APIs: ${result.newtonApiCount}`);
  }
  console.log(`MuJoCo model: ${result.model.nbody} bodies, ${result.model.njnt} joints, ${result.model.nq} qpos, dt=${result.model.timestep}`);
  console.log(`Mode: ${result.mode}, simulated ${result.seconds.toFixed(3)}s (${result.steps} steps)`);
  console.log(`Target: shoulder=${result.targetDeg[0].toFixed(2)} deg, elbow=${result.targetDeg[1].toFixed(2)} deg`);
  console.log(`Final: shoulder=${result.finalQDeg[0].toFixed(2)} deg, elbow=${result.finalQDeg[1].toFixed(2)} deg`);
  console.log(`Hold error: final=${result.finalErrorDeg.toFixed(3)} deg, peak=${result.peakErrorDeg.toFixed(3)} deg`);
  console.log(`Picker: x=${result.pickerPosition[0].toFixed(3)}, z=${result.pickerPosition[2].toFixed(3)}, minZ=${result.minPickerZ.toFixed(3)}`);
  if (result.mode === 'motor') {
    console.log(`Motor torque proxy peak: ${result.peakMotorTorque.toFixed(3)} N*m`);
  }
  if (opts.trajectoryPath && result.trajectory) {
    const dest = opts.trajectoryPath === '-' ? 'stdout' : path.relative(process.cwd(), opts.trajectoryPath);
    console.log(`Trajectory: ${result.trajectory.length} samples -> ${dest}`);
  }
  if (result.warn) {
    console.warn(result.warn);
  }
}

async function main() {
  const opts = parseArgs();
  const result = await runSimulation(opts);
  writeTrajectory(result, opts);
  if (opts.trajectoryPath !== '-') {
    printResult(result, opts);
  }
  if (!result.passed) {
    throw new Error(`physics hold failed: final error ${result.finalErrorDeg.toFixed(3)} deg, picker minZ ${result.minPickerZ.toFixed(3)}`);
  }
}

main().catch((err) => {
  console.error(`phys-sim: ${err.message}`);
  if (process.argv.includes('--verbose') || process.argv.includes('-v')) {
    console.error(err.stack);
  }
  process.exit(1);
});
