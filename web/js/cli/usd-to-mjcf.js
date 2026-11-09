#!/usr/bin/env node
// USD Physics -> MJCF (MuJoCo XML) return-leg exporter for LightUSD WASM.
//
// Loads a USD stage, pulls the physics scene back out via
// extractPhysicsSceneJSON(), and re-emits a structurally valid MJCF:
//   - PhysicsRigidBodyAPI prims              -> <body> (nested by joint tree)
//   - Physics*Joint prims                    -> <joint> (hinge/slide/free)
//   - Mesh / GeomSphere / GeomCube ... prims -> <geom>
//   - NewtonActuator prims                   -> <actuator><position .../>
//
// Mesh geoms are written out as companion .obj files (geometry is carried as
// raw vertices/indices through the physics JSON; the original file identity
// is not preserved by this path) so the emitted MJCF is self-contained and
// re-parseable. Pass --no-meshes to reference mesh names without writing OBJs.

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import LightUSDFactory from '../src/lightusd/lightusd.js';

const DEG_TO_RAD = Math.PI / 180;

function printHelp() {
  console.log(`
USD Physics -> MJCF (MuJoCo XML) return-leg exporter

Usage:
  node cli/usd-to-mjcf.js <scene.usd|usda|usdc|usdz> [options]

Options:
  -o, --output <path>  Output .mjcf/.xml path (default: <input>.roundtrip.mjcf)
  --model-name <name>  <mujoco model="..."> name (default: input basename)
  --placeholder-meshes  Write small OBJ placeholders without render extraction
  --no-meshes          Reference mesh names but do not write companion OBJ files
  --dump-json          Print the extracted physics scene JSON and exit
  -v, --verbose        Print conversion details
  -h, --help           Show this help
`);
}

function parseArgs(argv = process.argv.slice(2)) {
  const opts = {
    inputFile: null,
    outputFile: null,
    modelName: null,
    emitMeshes: true,
    placeholderMeshes: false,
    dumpJson: false,
    verbose: false
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      printHelp();
      process.exit(0);
    } else if (arg === '-o' || arg === '--output') {
      opts.outputFile = requireValue(argv, ++i, arg);
    } else if (arg === '--model-name') {
      opts.modelName = requireValue(argv, ++i, arg);
    } else if (arg === '--no-meshes') {
      opts.emitMeshes = false;
    } else if (arg === '--placeholder-meshes') {
      opts.placeholderMeshes = true;
    } else if (arg === '--dump-json') {
      opts.dumpJson = true;
    } else if (arg === '-v' || arg === '--verbose') {
      opts.verbose = true;
    } else if (arg.startsWith('-')) {
      throw new Error(`Unknown option: ${arg}`);
    } else if (!opts.inputFile) {
      opts.inputFile = arg;
    } else {
      throw new Error(`Unexpected argument: ${arg}`);
    }
  }
  if (!opts.inputFile) throw new Error('Input USD file is required.');
  if (opts.placeholderMeshes && !opts.emitMeshes) {
    throw new Error('--placeholder-meshes and --no-meshes cannot be combined.');
  }
  return opts;
}

function requireValue(argv, index, optionName) {
  const value = argv[index];
  if (!value || value.startsWith('-')) {
    throw new Error(`${optionName} requires a value`);
  }
  return value;
}

function fmtNumber(value) {
  if (!Number.isFinite(value)) return '0';
  return Number(value).toPrecision(9).replace(/(\.\d*?)0+($|\s)/u, '$1$2').replace(/\.$/u, '');
}

function vec(value, fallback = [0, 0, 0]) {
  const a = Array.isArray(value) ? value : fallback;
  return [a[0] || 0, a[1] || 0, a[2] || 0].map(fmtNumber).join(' ');
}

function escapeXML(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/"/g, '&quot;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

function basenameFromPath(primPath) {
  return String(primPath || '').split('/').filter(Boolean).pop() || '';
}

function hasApi(prim, apiName) {
  return (prim.apiSchemas || []).some((api) => api === apiName || api.startsWith(`${apiName}:`));
}

function firstRelTarget(prim, name) {
  const rel = prim.relationships?.[name];
  if (Array.isArray(rel)) return rel[0] || '';
  return '';
}

function matrixTranslation(matrix) {
  if (!Array.isArray(matrix) || matrix.length < 15) return [0, 0, 0];
  return [matrix[12] || 0, matrix[13] || 0, matrix[14] || 0];
}

function matrixScale(matrix) {
  if (!Array.isArray(matrix) || matrix.length < 11) return [1, 1, 1];
  return [
    Math.hypot(matrix[0] || 0, matrix[1] || 0, matrix[2] || 0) || 1,
    Math.hypot(matrix[4] || 0, matrix[5] || 0, matrix[6] || 0) || 1,
    Math.hypot(matrix[8] || 0, matrix[9] || 0, matrix[10] || 0) || 1
  ];
}

// Extract the rotation quaternion (MuJoCo wxyz order) from a column-major 4x4,
// dividing out per-axis scale. Returns [w, x, y, z].
function matrixToQuat(matrix) {
  if (!Array.isArray(matrix) || matrix.length < 11) return [1, 0, 0, 0];
  const s = matrixScale(matrix);
  // r_ij = element at row i, col j; column-major => matrix[j*4 + i].
  const r00 = (matrix[0] || 0) / s[0], r10 = (matrix[1] || 0) / s[0], r20 = (matrix[2] || 0) / s[0];
  const r01 = (matrix[4] || 0) / s[1], r11 = (matrix[5] || 0) / s[1], r21 = (matrix[6] || 0) / s[1];
  const r02 = (matrix[8] || 0) / s[2], r12 = (matrix[9] || 0) / s[2], r22 = (matrix[10] || 0) / s[2];
  const trace = r00 + r11 + r22;
  let w, x, y, z;
  if (trace > 0) {
    const S = Math.sqrt(trace + 1.0) * 2;
    w = 0.25 * S; x = (r21 - r12) / S; y = (r02 - r20) / S; z = (r10 - r01) / S;
  } else if (r00 > r11 && r00 > r22) {
    const S = Math.sqrt(1.0 + r00 - r11 - r22) * 2;
    w = (r21 - r12) / S; x = 0.25 * S; y = (r01 + r10) / S; z = (r02 + r20) / S;
  } else if (r11 > r22) {
    const S = Math.sqrt(1.0 + r11 - r00 - r22) * 2;
    w = (r02 - r20) / S; x = (r01 + r10) / S; y = 0.25 * S; z = (r12 + r21) / S;
  } else {
    const S = Math.sqrt(1.0 + r22 - r00 - r11) * 2;
    w = (r10 - r01) / S; x = (r02 + r20) / S; y = (r12 + r21) / S; z = 0.25 * S;
  }
  const n = Math.hypot(w, x, y, z) || 1;
  return [w / n, x / n, y / n, z / n];
}

// Whether a wxyz quaternion is (near) identity.
function isIdentityQuat(q) {
  return Math.abs(Math.abs(q[0]) - 1) < 1e-6 &&
    Math.abs(q[1]) < 1e-6 && Math.abs(q[2]) < 1e-6 && Math.abs(q[3]) < 1e-6;
}

// Longest link path that is a prefix of primPath.
function ownerLinkPath(primPath, linkPaths) {
  let best = null;
  for (const linkPath of linkPaths) {
    if (primPath === linkPath || primPath.startsWith(`${linkPath}/`)) {
      if (!best || linkPath.length > best.length) best = linkPath;
    }
  }
  return best;
}

const AXIS_VECTORS = { X: [1, 0, 0], Y: [0, 1, 0], Z: [0, 0, 1] };

function jointTypeToMujoco(type) {
  if (type === 'PhysicsRevoluteJoint') return 'hinge';
  if (type === 'PhysicsPrismaticJoint') return 'slide';
  if (type === 'PhysicsFixedJoint') return 'fixed';
  return 'fixed';
}

// Recover real mesh geometry from the render scene (populated by loadFromBinary)
// via getMesh(), keyed by absolute prim path. getMesh returns typed_memory_view
// windows into the WASM heap, so we copy them out before the native object is
// deleted. Points are link-local (the prim transform is baked by the render
// scene converter).
function collectRenderMeshes(native, warnings = []) {
  const map = new Map();
  const count = native.numMeshes ? native.numMeshes() : 0;
  for (let i = 0; i < count; i++) {
    try {
      const m = native.getMeshCopy(i);
      if (!m || !m.absPath || !m.points || m.points.length < 9) continue;
      map.set(m.absPath, {
        points: Float32Array.from(m.points),
        indices: Int32Array.from(m.faceVertexIndices || []),
        counts: Int32Array.from(m.faceVertexCounts || [])
      });
    } catch (error) {
      // Physics JSON remains usable when a malformed render-only UV array
      // prevents one mesh from materializing. The corresponding MJCF geom is
      // emitted with the existing placeholder fallback, while the warning is
      // surfaced to callers for follow-up.
      warnings.push(`mesh ${i}: ${error.message || error}`);
    }
  }
  return map;
}

// Build the structured robot model from the extracted physics JSON.
// `renderMeshes` (path -> {points, indices, counts}) carries real geometry
// recovered from the render scene via getMesh().
function buildModel(extracted, renderMeshes = new Map()) {
  const prims = extracted.prims || [];
  const linkPrims = prims.filter((prim) => hasApi(prim, 'PhysicsRigidBodyAPI'));
  const linkPaths = linkPrims.map((prim) => prim.path);

  const links = new Map();
  for (const prim of linkPrims) {
    links.set(prim.path, {
      path: prim.path,
      name: prim.name || basenameFromPath(prim.path),
      pos: matrixTranslation(prim.matrix),
      mass: Number(prim.properties?.['physics:mass'] || 0),
      com: prim.properties?.['physics:centerOfMass'] || [0, 0, 0],
      diagonalInertia: prim.properties?.['physics:diagonalInertia'] || null,
      visuals: [],
      collisions: [],
      outJoints: []
    });
  }

  // Geometry prims attach to the nearest enclosing link.
  for (const prim of prims) {
    if (!prim.geometry) continue;
    const linkPath = ownerLinkPath(prim.path, linkPaths);
    if (!linkPath) continue;
    const link = links.get(linkPath);
    const isCollision = hasApi(prim, 'PhysicsCollisionAPI');
    const geom = {
      name: prim.name || basenameFromPath(prim.path),
      pos: matrixTranslation(prim.matrix),
      scale: matrixScale(prim.matrix),
      quat: matrixToQuat(prim.matrix),
      geometry: prim.geometry,
      group: isCollision ? 3 : 2,
      // Real render geometry (link-local, prim transform already baked) for
      // mesh prims; primitive shapes don't need it.
      realMesh: prim.geometry.type === 'mesh' ? renderMeshes.get(prim.path) || null : null
    };
    if (isCollision) link.collisions.push(geom);
    else link.visuals.push(geom);
  }

  // Joints connect parent (body0) -> child (body1).
  const joints = [];
  for (const prim of prims) {
    if (!/Joint/.test(prim.type)) continue;
    const parentPath = firstRelTarget(prim, 'physics:body0');
    const childPath = firstRelTarget(prim, 'physics:body1');
    const axisToken = prim.properties?.['physics:axis'] || 'X';
    const mjType = jointTypeToMujoco(prim.type);
    const lowDeg = prim.properties?.[`physics:limit:rot${axisToken}:low`];
    const highDeg = prim.properties?.[`physics:limit:rot${axisToken}:high`];
    const lowLin = prim.properties?.[`physics:limit:trans${axisToken}:low`];
    const highLin = prim.properties?.[`physics:limit:trans${axisToken}:high`];
    let range = null;
    if (mjType === 'hinge' && Number.isFinite(lowDeg) && Number.isFinite(highDeg) && lowDeg !== highDeg) {
      range = [lowDeg * DEG_TO_RAD, highDeg * DEG_TO_RAD];
    } else if (mjType === 'slide' && Number.isFinite(lowLin) && Number.isFinite(highLin) && lowLin !== highLin) {
      range = [lowLin, highLin];
    }
    const joint = {
      name: prim.name || basenameFromPath(prim.path),
      mjType,
      parentPath,
      childPath,
      axis: AXIS_VECTORS[axisToken] || AXIS_VECTORS.X,
      pos: prim.properties?.['physics:localPos0'] || [0, 0, 0],
      range,
      damping: prim.properties?.['mjc:damping'],
      frictionloss: prim.properties?.['mjc:frictionloss'],
      armature: prim.properties?.['mjc:armature']
    };
    joints.push(joint);
    const parentLink = links.get(parentPath);
    if (parentLink) parentLink.outJoints.push(joint);
  }

  // Actuators reference a joint via newton:targets.
  const actuators = [];
  for (const prim of prims) {
    if (prim.type !== 'NewtonActuator') continue;
    const targetPath = firstRelTarget(prim, 'newton:targets');
    const jointName = basenameFromPath(targetPath);
    if (!jointName) continue;
    actuators.push({
      name: prim.name || `${jointName}_act`,
      joint: jointName,
      kp: Number(prim.properties?.['newton:kp'] || 0),
      kd: Number(prim.properties?.['newton:kd'] || 0),
      maxEffort: prim.properties?.['newton:maxEffort']
    });
  }

  // Roots are links never referenced as a joint child.
  const childPaths = new Set(joints.map((j) => j.childPath).filter(Boolean));
  const roots = [...links.values()].filter((link) => !childPaths.has(link.path));

  return { links, joints, actuators, roots, upAxis: extracted.upAxis || 'Z' };
}

function geomXML(geom, meshSink, indent) {
  const g = geom.geometry || {};
  const common = `name="${escapeXML(geom.name)}" group="${geom.group}"`;
  const pad = ' '.repeat(indent);
  const posAttr = `pos="${vec(geom.pos)}"`;
  // Orientation (MuJoCo wxyz quat) carried by the prim transform. Omitted for
  // spheres (rotation-invariant) and identity rotations.
  const q = geom.quat || [1, 0, 0, 0];
  const quatAttr = isIdentityQuat(q)
    ? ''
    : ` quat="${q.map(fmtNumber).join(' ')}"`;
  const xform = `${posAttr}${quatAttr}`;

  if (g.type === 'sphere') {
    return `${pad}<geom ${common} type="sphere" ${posAttr} size="${fmtNumber(g.radius || 0.01)}"/>`;
  }
  if (g.type === 'box' || g.type === 'cube') {
    const base = Array.isArray(g.size)
      ? g.size
      : (typeof g.size === 'number' ? [g.size, g.size, g.size] : [2, 2, 2]);
    const half = base.map((v, i) => (Number(v || 1) * (geom.scale[i] || 1)) * 0.5);
    return `${pad}<geom ${common} type="box" ${xform} size="${vec(half, [0.5, 0.5, 0.5])}"/>`;
  }
  if (g.type === 'cylinder' || g.type === 'capsule') {
    const r = fmtNumber(g.radius || 0.01);
    const halfLen = fmtNumber((Number(g.length || g.height || 0.02)) * 0.5);
    return `${pad}<geom ${common} type="${g.type}" ${xform} size="${r} ${halfLen}"/>`;
  }
  if (g.type === 'plane') {
    const hw = fmtNumber((Number(g.width || 1)) * 0.5);
    const hl = fmtNumber((Number(g.length || 1)) * 0.5);
    return `${pad}<geom ${common} type="plane" ${xform} size="${hw} ${hl} 0.1"/>`;
  }
  // mesh: real render geometry is in link-local space (prim transform baked),
  // so it sits at the body origin; placeholders keep the prim translation.
  const meshName = meshSink.add(geom);
  const meshPos = geom.realMesh ? 'pos="0 0 0"' : posAttr;
  return `${pad}<geom ${common} type="mesh" ${meshPos} mesh="${escapeXML(meshName)}"/>`;
}

function jointXML(joint, indent) {
  if (joint.mjType === 'fixed') return null; // a fixed joint = nested body, no <joint>
  const pad = ' '.repeat(indent);
  const attrs = [
    `name="${escapeXML(joint.name)}"`,
    `type="${joint.mjType}"`,
    `axis="${vec(joint.axis, [1, 0, 0])}"`
  ];
  if (joint.range) attrs.push(`range="${fmtNumber(joint.range[0])} ${fmtNumber(joint.range[1])}"`);
  if (Number.isFinite(joint.damping) && joint.damping !== 0) attrs.push(`damping="${fmtNumber(joint.damping)}"`);
  if (Number.isFinite(joint.frictionloss) && joint.frictionloss !== 0) attrs.push(`frictionloss="${fmtNumber(joint.frictionloss)}"`);
  if (Number.isFinite(joint.armature) && joint.armature !== 0) attrs.push(`armature="${fmtNumber(joint.armature)}"`);
  return `${pad}<joint ${attrs.join(' ')}/>`;
}

function inertialXML(link, indent) {
  if (!(link.mass > 0)) return null;
  const pad = ' '.repeat(indent);
  const di = Array.isArray(link.diagonalInertia) && link.diagonalInertia.length >= 3 && link.diagonalInertia.every((v) => v > 0)
    ? link.diagonalInertia
    : [1e-4, 1e-4, 1e-4];
  return `${pad}<inertial pos="${vec(link.com)}" mass="${fmtNumber(link.mass)}" diaginertia="${vec(di, [1e-4, 1e-4, 1e-4])}"/>`;
}

// Unit cube (half-extent 0.05) used as a placeholder for mesh geoms whose
// vertex data is not carried by extractPhysicsSceneJSON.
const PLACEHOLDER_CUBE = (() => {
  const h = 0.05;
  const v = [
    [-h, -h, -h], [h, -h, -h], [h, h, -h], [-h, h, -h],
    [-h, -h, h], [h, -h, h], [h, h, h], [-h, h, h]
  ];
  const f = [
    [1, 2, 3], [1, 3, 4], [5, 7, 6], [5, 8, 7],
    [1, 5, 6], [1, 6, 2], [2, 6, 7], [2, 7, 3],
    [3, 7, 8], [3, 8, 4], [4, 8, 5], [4, 5, 1]
  ];
  return { v, f };
})();

class MeshSink {
  constructor() {
    this.meshes = []; // { name, pointCount, faceCount, placeholder }
    this.seen = new Set();
  }

  add(geom) {
    let name = geom.name || `mesh_${this.meshes.length}`;
    if (this.seen.has(name)) name = `${name}_${this.meshes.length}`;
    this.seen.add(name);
    const real = geom.realMesh;
    this.meshes.push({
      name,
      real: real || null,
      pointCount: real ? real.points.length / 3 : (geom.geometry?.pointCount ?? 0),
      faceCount: real ? real.counts.length : (geom.geometry?.faceCount ?? 0),
      placeholder: !real
    });
    return name;
  }
}

function objText(mesh) {
  // Real render geometry (recovered via getMesh) is written verbatim. When it is
  // unavailable we fall back to a small placeholder cube to keep the MJCF loadable.
  const lines = [`# ${mesh.name} (points=${mesh.pointCount}, faces=${mesh.faceCount}${mesh.placeholder ? ', PLACEHOLDER cube — render mesh not found' : ''})`];
  if (mesh.placeholder) {
    for (const p of PLACEHOLDER_CUBE.v) lines.push(`v ${p[0]} ${p[1]} ${p[2]}`);
    for (const f of PLACEHOLDER_CUBE.f) lines.push(`f ${f[0]} ${f[1]} ${f[2]}`);
    return `${lines.join('\n')}\n`;
  }
  const { points, indices, counts } = mesh.real;
  for (let i = 0; i + 2 < points.length; i += 3) {
    lines.push(`v ${fmtNumber(points[i])} ${fmtNumber(points[i + 1])} ${fmtNumber(points[i + 2])}`);
  }
  // Emit one face per faceVertexCount run (handles tris and n-gons).
  let cursor = 0;
  for (let f = 0; f < counts.length; f++) {
    const c = counts[f];
    if (c >= 3 && cursor + c <= indices.length) {
      const verts = [];
      for (let k = 0; k < c; k++) verts.push(indices[cursor + k] + 1);
      lines.push(`f ${verts.join(' ')}`);
    }
    cursor += c;
  }
  return `${lines.join('\n')}\n`;
}

function emitMJCF(model, opts, meshDirName) {
  const meshSink = new MeshSink();
  const lines = [];
  let bodyCount = 0;
  let jointCount = 0;
  let visualCount = 0;
  let collisionCount = 0;

  function emitBody(link, joint, indent) {
    bodyCount++;
    const pad = ' '.repeat(indent);
    const pos = joint ? joint.pos : link.pos;
    lines.push(`${pad}<body name="${escapeXML(link.name)}" pos="${vec(pos)}">`);

    const inertial = inertialXML(link, indent + 2);
    if (inertial) lines.push(inertial);

    if (joint) {
      // A fixed joint is a nested body with no <joint> element, but it still
      // counts as a joint edge (matches the forward leg, which emits one joint
      // per child body including fixed ones).
      const jx = jointXML(joint, indent + 2);
      if (jx) lines.push(jx);
    }

    for (const g of link.visuals) {
      lines.push(geomXML(g, meshSink, indent + 2));
      visualCount++;
    }
    for (const g of link.collisions) {
      lines.push(geomXML(g, meshSink, indent + 2));
      collisionCount++;
    }

    for (const childJoint of link.outJoints) {
      const childLink = model.links.get(childJoint.childPath);
      if (childLink) {
        jointCount++;
        emitBody(childLink, childJoint, indent + 2);
      }
    }

    lines.push(`${pad}</body>`);
  }

  for (const root of model.roots) emitBody(root, null, 4);

  const placeholderMeshes = meshSink.meshes.filter((m) => m.placeholder).length;

  const header = [];
  header.push(`<mujoco model="${escapeXML(opts.modelName)}">`);
  const realMeshes = meshSink.meshes.length - placeholderMeshes;
  header.push('  <!-- Generated by usd-to-mjcf.js (USD Physics -> MJCF return leg).');
  header.push('       Kinematic structure (bodies/joints), primitive collision shapes,');
  header.push('       inertials and actuators round-trip through USD. Mesh vertex data is');
  header.push('       recovered from the render scene via getMesh() and written to OBJ');
  header.push(`       (${realMeshes} real mesh(es), ${placeholderMeshes} placeholder cube(s) where unavailable). -->`);
  header.push(`  <compiler angle="radian" meshdir="${meshDirName}" autolimits="true"/>`);
  header.push(`  <option integrator="implicitfast"/>`);

  // Asset block for mesh geoms.
  const assetLines = [];
  if (meshSink.meshes.length) {
    assetLines.push('  <asset>');
    for (const m of meshSink.meshes) {
      assetLines.push(`    <mesh name="${escapeXML(m.name)}" file="${escapeXML(m.name)}.obj"/>`);
    }
    assetLines.push('  </asset>');
  }

  const actuatorLines = [];
  if (model.actuators.length) {
    actuatorLines.push('  <actuator>');
    for (const a of model.actuators) {
      const kp = Number.isFinite(a.kp) && a.kp > 0 ? ` kp="${fmtNumber(a.kp)}"` : '';
      const kd = Number.isFinite(a.kd) && a.kd > 0 ? ` kv="${fmtNumber(a.kd)}"` : '';
      const fr = Number.isFinite(a.maxEffort) && a.maxEffort > 0
        ? ` forcerange="${fmtNumber(-a.maxEffort)} ${fmtNumber(a.maxEffort)}"`
        : '';
      actuatorLines.push(`    <position name="${escapeXML(a.name)}" joint="${escapeXML(a.joint)}"${kp}${kd}${fr}/>`);
    }
    actuatorLines.push('  </actuator>');
  }

  const xml = [
    ...header,
    ...assetLines,
    '  <worldbody>',
    ...lines,
    '  </worldbody>',
    ...actuatorLines,
    '</mujoco>',
    ''
  ].join('\n');

  return {
    xml,
    meshes: meshSink.meshes,
    stats: {
      bodies: bodyCount,
      joints: jointCount,
      visuals: visualCount,
      collisions: collisionCount,
      actuators: model.actuators.length,
      meshGeoms: meshSink.meshes.length,
      placeholderMeshes
    }
  };
}

async function main() {
  const opts = parseArgs();
  const inputPath = path.resolve(opts.inputFile);
  const bytes = fs.readFileSync(inputPath);
  const baseName = path.basename(inputPath, path.extname(inputPath));
  opts.modelName = opts.modelName || baseName;

  const lightusd = await LightUSDFactory();
  const native = new lightusd.LightUSDLoaderNative();
  let extracted;
  let renderMeshes = new Map();
  const renderWarnings = [];
  try {
    // Physics extraction only needs the pristine layer. The ordinary stage
    // load eagerly runs Tydra conversion, so one malformed render-only UV
    // array would incorrectly make a structurally valid physics return-leg
    // conversion fail before extractPhysicsSceneJSON can run.
    if (!native.loadAsLayerFromBinary(bytes, path.basename(inputPath))) {
      throw new Error(native.error() || `Failed to load ${inputPath}`);
    }
    const jsonText = native.extractPhysicsSceneJSON();
    if (!jsonText) throw new Error(native.error() || 'extractPhysicsSceneJSON failed');
    extracted = JSON.parse(jsonText);
    if (opts.emitMeshes && !opts.placeholderMeshes) {
      if (native.layerToRenderScene()) {
        renderMeshes = collectRenderMeshes(native, renderWarnings);
      } else {
        renderWarnings.push(native.error() || 'LayerToRenderScene failed');
      }
    }
  } finally {
    native.delete();
  }

  if (opts.dumpJson) {
    console.log(JSON.stringify(extracted, null, 2));
    return;
  }

  const model = buildModel(extracted, renderMeshes);

  const outPath = opts.outputFile
    ? path.resolve(opts.outputFile)
    : `${inputPath.replace(/\.[^.]+$/u, '')}.roundtrip.mjcf`;
  const outDir = path.dirname(outPath);
  const meshDirName = `${path.basename(outPath, path.extname(outPath))}_assets`;

  const { xml, meshes, stats } = emitMJCF(model, opts, meshDirName);

  fs.mkdirSync(outDir, { recursive: true });
  fs.writeFileSync(outPath, xml, 'utf8');

  if (opts.emitMeshes && meshes.length) {
    const meshDir = path.join(outDir, meshDirName);
    fs.mkdirSync(meshDir, { recursive: true });
    for (const m of meshes) {
      fs.writeFileSync(path.join(meshDir, `${m.name}.obj`), objText(m), 'utf8');
    }
  }

  console.log(`Wrote ${outPath}: ${stats.bodies} bodies, ${stats.joints} joints, ${stats.visuals} visuals, ${stats.collisions} collisions, ${stats.actuators} actuators.`);
  if (opts.verbose) {
    console.log(`upAxis=${model.upAxis}, roots=${model.roots.map((r) => r.name).join(', ')}`);
    console.log(`mesh geoms=${stats.meshGeoms} (real=${stats.meshGeoms - stats.placeholderMeshes}, placeholder=${stats.placeholderMeshes}), meshes written=${opts.emitMeshes ? meshes.length : 0}`);
  }
  if (renderWarnings.length) {
    console.warn(`Render mesh warnings: ${renderWarnings.length} (placeholder OBJ fallback used)`);
    if (opts.verbose) for (const warning of renderWarnings) console.warn(`  ${warning}`);
  }
}

main().catch((err) => {
  console.error(`usd-to-mjcf: ${err.message}`);
  if (process.argv.includes('--verbose') || process.argv.includes('-v')) {
    console.error(err.stack);
  }
  process.exit(1);
});
