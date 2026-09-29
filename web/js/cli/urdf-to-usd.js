#!/usr/bin/env node
// URDF/MJCF -> USD Physics + MuJoCo + Newton export tester for LightUSD WASM.

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import * as THREE from 'three';
import { OBJLoader } from 'three/examples/jsm/loaders/OBJLoader.js';
import { STLLoader } from 'three/examples/jsm/loaders/STLLoader.js';
import { LightUSDLoader } from '../src/lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from '../src/lightusd/LightUSDLoaderUtils.js';
import { weldMeshGeometry } from '../src/mjcf-mesh-weld.js';
import LightUSDFactory from '../src/lightusd/lightusd.js';

const SUPPORTED_FORMATS = new Set(['usda', 'usdc', 'usdz', 'all']);
const USD_MESH_EXTENSIONS = new Set(['.usd', '.usda', '.usdc', '.usdz']);

const SAMPLE_URDF = `<?xml version="1.0"?>
<robot name="LightUSDSampleRobot">
  <link name="base_link">
    <inertial>
      <origin xyz="0 0 0"/>
      <mass value="1"/>
      <inertia ixx="1" iyy="1" izz="1"/>
    </inertial>
    <visual name="visual_mesh">
      <origin xyz="0 0 0" rpy="0 0 0"/>
      <geometry>
        <mesh filename="sample.obj" scale="1 1 1"/>
      </geometry>
    </visual>
    <collision name="collision_box">
      <origin xyz="0 0 0" rpy="0 0 0"/>
      <geometry>
        <box size="1 1 0.3"/>
      </geometry>
    </collision>
  </link>
  <link name="arm_link">
    <inertial>
      <origin xyz="0 0 0"/>
      <mass value="0.25"/>
      <inertia ixx="0.1" iyy="0.1" izz="0.1"/>
    </inertial>
    <visual name="arm_box">
      <origin xyz="0.4 0 0" rpy="0 0 0"/>
      <geometry>
        <box size="0.8 0.18 0.18"/>
      </geometry>
    </visual>
  </link>
  <joint name="hinge" type="revolute">
    <parent link="base_link"/>
    <child link="arm_link"/>
    <origin xyz="0 0 0.2" rpy="0 0 0"/>
    <axis xyz="0 0 1"/>
    <limit lower="-1.57" upper="1.57" effort="2" velocity="4"/>
    <dynamics damping="0.1" friction="0.02"/>
  </joint>
</robot>
`;

const SAMPLE_OBJ = `o sample_triangle
v 0 0 0
v 0.7 0 0
v 0 0.7 0
vn 0 0 1
vt 0 0
vt 1 0
vt 0 1
f 1/1/1 2/2/1 3/3/1
`;

function printHelp() {
  console.log(`
URDF/MJCF -> USD Physics + MuJoCo + Newton CLI tester

Usage:
  node cli/urdf-to-usd.js <robot.urdf> [options]
  node cli/urdf-to-usd.js <scene.xml> --input-format mjcf [options]
  node cli/urdf-to-usd.js --sample --format all -o /tmp/robot

Options:
  --format <fmt>       Export format: usda, usdc, usdz, all (default: usda)
  --input-format <fmt> Input format: auto, urdf, mjcf (default: auto)
  -o, --output <path>  Output file path, or base path when --format all
  --asset-dir <dir>    Add a directory used to resolve mesh assets
  --package-root <dir> Resolve package:// URIs under this directory
  --up-axis <axis>     Export up axis: Z or Y (default: Z)
  --allow-missing      Skip missing/unsupported meshes instead of failing
  --allow-unsafe-paths Allow trusted XML to read assets/includes outside input
                       dir, --asset-dir, and --package-root
  --tessellate-collision-shapes
                       Tessellate primitive collision shapes to mesh.
                       Default: use USD native shape prims for primitive collisions.
  --max-usdc-mb <N>    Raise the USDC writer's max output size to N MB
                       (default WASM cap is 100MB). Use for mesh-dense scenes.
  --max-mem-mb <N>     Raise the USDC writer's max memory estimate to N MB.
  --dump-json <path>   Write the generated createURDFPhysicsScene JSON payload
  --no-verify          Do not verify expected USD/MuJoCo schema text
  --sample             Use an embedded URDF + OBJ smoke-test robot
  -v, --verbose        Print mesh and export details
  -h, --help           Show this help
`);
}

function parseArgs(argv = process.argv.slice(2)) {
  const opts = {
    inputFile: null,
    inputFormat: 'auto',
    format: 'usda',
    outputFile: null,
    assetDirs: [],
    packageRoot: null,
    upAxis: 'Z',
    allowMissing: false,
    allowUnsafePaths: false,
    tessellateCollisionShapes: false,
    dumpJson: null,
    maxUsdcMb: 0,
    maxMemMb: 0,
    verify: true,
    sample: false,
    verbose: false
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      printHelp();
      process.exit(0);
    } else if (arg === '--format') {
      opts.format = requireValue(argv, ++i, arg).toLowerCase();
      if (!SUPPORTED_FORMATS.has(opts.format)) {
        throw new Error('--format must be one of: usda, usdc, usdz, all');
      }
    } else if (arg === '--input-format') {
      opts.inputFormat = requireValue(argv, ++i, arg).toLowerCase();
      if (!['auto', 'urdf', 'mjcf'].includes(opts.inputFormat)) {
        throw new Error('--input-format must be one of: auto, urdf, mjcf');
      }
    } else if (arg === '-o' || arg === '--output') {
      opts.outputFile = requireValue(argv, ++i, arg);
    } else if (arg === '--asset-dir') {
      opts.assetDirs.push(requireValue(argv, ++i, arg));
    } else if (arg === '--package-root') {
      opts.packageRoot = requireValue(argv, ++i, arg);
    } else if (arg === '--up-axis') {
      opts.upAxis = requireValue(argv, ++i, arg).toUpperCase();
      if (opts.upAxis !== 'Z' && opts.upAxis !== 'Y') {
        throw new Error('--up-axis must be Z or Y');
      }
    } else if (arg === '--allow-missing') {
      opts.allowMissing = true;
    } else if (arg === '--allow-unsafe-paths') {
      opts.allowUnsafePaths = true;
    } else if (arg === '--tessellate-collision-shapes') {
      opts.tessellateCollisionShapes = true;
    } else if (arg === '--max-usdc-mb') {
      opts.maxUsdcMb = Number(requireValue(argv, ++i, arg)) || 0;
    } else if (arg === '--max-mem-mb') {
      opts.maxMemMb = Number(requireValue(argv, ++i, arg)) || 0;
    } else if (arg === '--dump-json') {
      opts.dumpJson = requireValue(argv, ++i, arg);
    } else if (arg === '--no-verify') {
      opts.verify = false;
    } else if (arg === '--sample') {
      opts.sample = true;
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

  if (!opts.inputFile && !opts.sample) {
    throw new Error('Input URDF is required, or use --sample.');
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

function realpathOrResolved(value) {
  const resolved = path.resolve(value);
  try {
    return fs.realpathSync(resolved);
  } catch {
    return resolved;
  }
}

function isPathInside(candidate, root) {
  const rel = path.relative(root, candidate);
  return rel === '' || (rel && !rel.startsWith('..') && !path.isAbsolute(rel));
}

function readRoots(opts, inputDir) {
  const roots = [inputDir, ...(opts?.assetDirs || [])];
  if (opts?.packageRoot) roots.push(opts.packageRoot);
  return Array.from(new Set(roots.filter(Boolean).map(realpathOrResolved)));
}

function assertSafeReadPath(filePath, opts, inputDir, label = 'asset') {
  if (opts?.allowUnsafePaths) return path.resolve(filePath);
  const resolved = realpathOrResolved(filePath);
  const roots = readRoots(opts, inputDir);
  if (roots.some((root) => isPathInside(resolved, root))) return resolved;
  throw new Error(
    `Blocked ${label} outside allowed roots: ${path.resolve(filePath)}. ` +
    'Use --asset-dir/--package-root for trusted asset roots, or ' +
    '--allow-unsafe-paths for legacy trusted XML.'
  );
}

function optsWithDefaultReadRoot(opts = {}, inputDir) {
  return {
    ...opts,
    assetDirs: (opts.assetDirs && opts.assetDirs.length) ? opts.assetDirs : [inputDir]
  };
}

function parseAttributes(text = '') {
  const attrs = {};
  const re = /([A-Za-z_:][-A-Za-z0-9_:.]*)\s*=\s*(?:"([^"]*)"|'([^']*)')/g;
  let match = null;
  while ((match = re.exec(text))) {
    attrs[match[1]] = decodeXML(match[2] ?? match[3] ?? '');
  }
  return attrs;
}

function decodeXML(text) {
  return text
    .replace(/&quot;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&');
}

function findElements(xml, tagName) {
  const elements = [];
  const re = new RegExp(`<${tagName}\\b([^>]*?)(?:/\\s*>|>([\\s\\S]*?)</${tagName}\\s*>)`, 'gi');
  let match = null;
  while ((match = re.exec(xml))) {
    elements.push({
      attrs: parseAttributes(match[1] || ''),
      body: match[2] || ''
    });
  }
  return elements;
}

function firstElement(xml, tagName) {
  return findElements(xml, tagName)[0] || null;
}

function parseXMLTree(xml) {
  const root = { name: '#document', attrs: {}, children: [], parent: null };
  const stack = [root];
  const tokenRe = /<!--[\s\S]*?-->|<\?[\s\S]*?\?>|<!\[CDATA\[[\s\S]*?\]\]>|<![^>]*>|<\/\s*([A-Za-z_:][-A-Za-z0-9_:.]*)\s*>|<\s*([A-Za-z_:][-A-Za-z0-9_:.]*)([^>]*?)(\/?)>/g;
  let match = null;
  while ((match = tokenRe.exec(xml))) {
    if (!match[0].startsWith('<') || match[0].startsWith('<!--') || match[0].startsWith('<?') || match[0].startsWith('<!')) {
      continue;
    }
    if (match[1]) {
      const closing = match[1];
      while (stack.length > 1 && stack[stack.length - 1].name !== closing) {
        stack.pop();
      }
      if (stack.length > 1) stack.pop();
      continue;
    }
    const node = {
      name: match[2],
      attrs: parseAttributes(match[3] || ''),
      children: [],
      parent: stack[stack.length - 1]
    };
    stack[stack.length - 1].children.push(node);
    if (!match[4]) stack.push(node);
  }
  return root.children[0] || root;
}

function childElements(node, name = null) {
  return (node?.children || []).filter((child) => !name || child.name === name);
}

function firstChild(node, name) {
  return childElements(node, name)[0] || null;
}

function numberAttr(attrs, name, fallback = undefined) {
  const value = Number(attrs?.[name]);
  return Number.isFinite(value) ? value : fallback;
}

function parseNumbers(text, fallback = []) {
  if (!text) return fallback;
  const values = text.trim().split(/\s+/).map(Number).filter(Number.isFinite);
  return values.length ? values : fallback;
}

function elementText(el) {
  return (el?.body || '').replace(/<[^>]*>/g, ' ').trim();
}

function matrixToUSDArray(matrix) {
  const e = matrix.elements;
  return [
    e[0], e[1], e[2], e[3],
    e[4], e[5], e[6], e[7],
    e[8], e[9], e[10], e[11],
    e[12], e[13], e[14], e[15]
  ];
}

// MuJoCo <compiler> context for angle units + euler sequence. Set per-parse in
// buildMujocoPayload; defaults match MuJoCo (degrees, "xyz").
let mjcfPoseCtx = { toRad: Math.PI / 180, eulerseq: 'xyz' };

function eulerQuatFromSeq(angles, seq, toRad) {
  const axisFor = (c) => (c === 'x' ? new THREE.Vector3(1, 0, 0)
    : c === 'y' ? new THREE.Vector3(0, 1, 0) : new THREE.Vector3(0, 0, 1));
  const q = new THREE.Quaternion();
  for (let i = 0; i < seq.length && i < angles.length; i++) {
    const c = seq[i];
    const lower = c.toLowerCase();
    const qi = new THREE.Quaternion().setFromAxisAngle(axisFor(lower), (angles[i] || 0) * toRad);
    if (c === lower) q.multiply(qi);   // lowercase = intrinsic (moving axes)
    else q.premultiply(qi);            // uppercase = extrinsic (fixed axes)
  }
  return q;
}

// Resolve any MuJoCo orientation specifier (quat/axisangle/euler/xyaxes/zaxis).
function orientationQuat(attrs, ctx) {
  const toRad = ctx.toRad;
  if (attrs.quat) {
    const q = parseNumbers(attrs.quat, [1, 0, 0, 0]);
    return new THREE.Quaternion(q[1] || 0, q[2] || 0, q[3] || 0, q[0] ?? 1).normalize();
  }
  if (attrs.axisangle) {
    const a = parseNumbers(attrs.axisangle, [0, 0, 1, 0]);
    const axis = new THREE.Vector3(a[0] || 0, a[1] || 0, a[2] || 0);
    if (axis.lengthSq() < 1e-12) axis.set(0, 0, 1);
    return new THREE.Quaternion().setFromAxisAngle(axis.normalize(), (a[3] || 0) * toRad);
  }
  if (attrs.euler) {
    return eulerQuatFromSeq(parseNumbers(attrs.euler, [0, 0, 0]), ctx.eulerseq, toRad);
  }
  if (attrs.xyaxes) {
    const v = parseNumbers(attrs.xyaxes, [1, 0, 0, 0, 1, 0]);
    const x = new THREE.Vector3(v[0], v[1], v[2]);
    if (x.lengthSq() < 1e-12) x.set(1, 0, 0);
    x.normalize();
    const y = new THREE.Vector3(v[3], v[4], v[5]);
    y.sub(x.clone().multiplyScalar(x.dot(y)));   // Gram-Schmidt against x
    if (y.lengthSq() < 1e-12) y.crossVectors(new THREE.Vector3(0, 0, 1), x);
    y.normalize();
    const z = new THREE.Vector3().crossVectors(x, y);
    return new THREE.Quaternion().setFromRotationMatrix(new THREE.Matrix4().makeBasis(x, y, z));
  }
  if (attrs.zaxis) {
    const v = parseNumbers(attrs.zaxis, [0, 0, 1]);
    const z = new THREE.Vector3(v[0] || 0, v[1] || 0, v[2] || 0);
    if (z.lengthSq() < 1e-12) z.set(0, 0, 1);
    return new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 0, 1), z.normalize());
  }
  return new THREE.Quaternion();
}

function matrixFromPoseAttrs(attrs = {}) {
  const pos = parseNumbers(attrs.pos, [0, 0, 0]);
  const translation = new THREE.Vector3(pos[0] || 0, pos[1] || 0, pos[2] || 0);
  const quat = orientationQuat(attrs, mjcfPoseCtx);
  return new THREE.Matrix4().compose(translation, quat, new THREE.Vector3(1, 1, 1));
}

// Dissolve MuJoCo 3 <frame> grouping transforms: compose each frame's transform
// into its children's poses and lift them to the frame's parent (mirrors the
// C++ FlattenFramesIn / urdf.js flattenMujocoFrames). Needs mjcfPoseCtx set.
function flattenMujocoFramesTree(parent) {
  for (const c of parent.children) flattenMujocoFramesTree(c);
  const out = [];
  for (const child of parent.children) {
    if (child.name !== 'frame') { out.push(child); continue; }
    const fm = matrixFromPoseAttrs(child.attrs);
    const fcc = child.attrs.childclass;
    for (const gc of child.children) {
      const cm = new THREE.Matrix4().multiplyMatrices(fm, matrixFromPoseAttrs(gc.attrs));
      const pos = new THREE.Vector3(), quat = new THREE.Quaternion(), scl = new THREE.Vector3();
      cm.decompose(pos, quat, scl);
      gc.attrs.pos = `${pos.x} ${pos.y} ${pos.z}`;
      gc.attrs.quat = `${quat.w} ${quat.x} ${quat.y} ${quat.z}`;  // MuJoCo wxyz
      delete gc.attrs.euler; delete gc.attrs.axisangle; delete gc.attrs.xyaxes; delete gc.attrs.zaxis;
      const ft = parseNumbers(gc.attrs.fromto, []);
      if (ft.length === 6) {
        const p0 = new THREE.Vector3(ft[0], ft[1], ft[2]).applyMatrix4(fm);
        const p1 = new THREE.Vector3(ft[3], ft[4], ft[5]).applyMatrix4(fm);
        gc.attrs.fromto = `${p0.x} ${p0.y} ${p0.z} ${p1.x} ${p1.y} ${p1.z}`;
      }
      if (fcc && !gc.attrs.childclass && !gc.attrs.class) gc.attrs.childclass = fcc;
      gc.parent = parent;
      out.push(gc);
    }
  }
  parent.children = out;
}

function originToMatrix(originAttrs = {}) {
  const xyz = parseNumbers(originAttrs.xyz, [0, 0, 0]);
  const rpy = parseNumbers(originAttrs.rpy, [0, 0, 0]);
  return new THREE.Matrix4().compose(
    new THREE.Vector3(xyz[0] || 0, xyz[1] || 0, xyz[2] || 0),
    new THREE.Quaternion().setFromEuler(new THREE.Euler(rpy[0] || 0, rpy[1] || 0, rpy[2] || 0, 'XYZ')),
    new THREE.Vector3(1, 1, 1)
  );
}

function axisToToken(axis) {
  const abs = axis.map((v) => Math.abs(v));
  const max = Math.max(abs[0] || 0, abs[1] || 0, abs[2] || 0);
  if (max === abs[1]) return 'Y';
  if (max === abs[2]) return 'Z';
  return 'X';
}

function parseURDFMetadata(urdfText) {
  const robotEl = firstElement(urdfText, 'robot');
  const links = new Map();
  const joints = [];
  const actuators = [];

  for (const linkEl of findElements(urdfText, 'link')) {
    const name = linkEl.attrs.name || `link_${links.size}`;
    const inertialEl = firstElement(linkEl.body, 'inertial');
    const inertial = {};
    if (inertialEl) {
      const massEl = firstElement(inertialEl.body, 'mass');
      const originEl = firstElement(inertialEl.body, 'origin');
      const inertiaEl = firstElement(inertialEl.body, 'inertia');
      if (massEl) inertial.mass = numberAttr(massEl.attrs, 'value', 0);
      if (originEl) inertial.centerOfMass = parseNumbers(originEl.attrs.xyz, [0, 0, 0]);
      if (inertiaEl) {
        inertial.diagonalInertia = [
          numberAttr(inertiaEl.attrs, 'ixx', 0),
          numberAttr(inertiaEl.attrs, 'iyy', 0),
          numberAttr(inertiaEl.attrs, 'izz', 0)
        ];
      }
    }
    links.set(name, { name, inertial, body: linkEl.body });
  }

  for (const jointEl of findElements(urdfText, 'joint')) {
    const parent = firstElement(jointEl.body, 'parent')?.attrs.link || '';
    const child = firstElement(jointEl.body, 'child')?.attrs.link || '';
    const axis = parseNumbers(firstElement(jointEl.body, 'axis')?.attrs.xyz, [1, 0, 0]);
    const originEl = firstElement(jointEl.body, 'origin');
    const origin = parseNumbers(originEl?.attrs.xyz, [0, 0, 0]);
    const limitEl = firstElement(jointEl.body, 'limit');
    const dynamicsEl = firstElement(jointEl.body, 'dynamics');
    const joint = {
      name: jointEl.attrs.name || `joint_${joints.length}`,
      type: jointEl.attrs.type || 'fixed',
      parent,
      child,
      axis,
      axisToken: axisToToken(axis),
      origin,
      originMatrix: matrixToUSDArray(originToMatrix(originEl?.attrs)),
      limit: limitEl ? {
        lower: numberAttr(limitEl.attrs, 'lower'),
        upper: numberAttr(limitEl.attrs, 'upper'),
        effort: numberAttr(limitEl.attrs, 'effort'),
        velocity: numberAttr(limitEl.attrs, 'velocity')
      } : {},
      // URDF 1.0 <dynamics> spec only defines damping + friction, but
      // we accept the common MuJoCo extensions stiffness and armature
      // when authored (no-op for compliant URDFs). The downstream
      // C++ converter (urdf-to-usd.cc) emits these via the PhysX
      // mirror schema (physxJoint:armature, physxLimit:angular:stiffness)
      // alongside the canonical mjc:* fallbacks.
      dynamics: dynamicsEl ? {
        damping: numberAttr(dynamicsEl.attrs, 'damping'),
        friction: numberAttr(dynamicsEl.attrs, 'friction'),
        stiffness: numberAttr(dynamicsEl.attrs, 'stiffness'),
        armature: numberAttr(dynamicsEl.attrs, 'armature')
      } : {}
    };
    const mimicEl = firstElement(jointEl.body, 'mimic');
    if (mimicEl?.attrs?.joint) {
      joint.mimic = {
        joint: mimicEl.attrs.joint,
        multiplier: numberAttr(mimicEl.attrs, 'multiplier', 1),
        offset: numberAttr(mimicEl.attrs, 'offset', 0)
      };
      // The C++ converter maps this to NewtonMimicAPI when the target joint is
      // exported.
    }
    joints.push(joint);
  }

  const jointsByName = new Map(joints.map((joint) => [joint.name, joint]));
  for (const transmissionEl of findElements(urdfText, 'transmission')) {
    const jointName = firstElement(transmissionEl.body, 'joint')?.attrs.name || '';
    if (!jointName) continue;
    const actuatorEl = firstElement(transmissionEl.body, 'actuator');
    const reduction = Number(elementText(firstElement(actuatorEl?.body || '', 'mechanicalReduction')));
    const joint = jointsByName.get(jointName);
    const act = {
      name: actuatorEl?.attrs.name || `${jointName}_actuator`,
      joint: jointName,
      control: 'pd'
    };
    const effort = joint?.limit?.effort;
    if (Number.isFinite(effort) && effort > 0) act.maxEffort = effort;
    if (Number.isFinite(reduction) && reduction !== 0) act.constEffort = Math.abs(reduction);
    actuators.push(act);
  }

  return {
    name: robotEl?.attrs.name || '',
    links,
    joints,
    actuators
  };
}

class MeshResolver {
  constructor(opts, urdfDir) {
    this.opts = opts;
    this.urdfDir = urdfDir;
    this.virtualFiles = new Map();
    if (opts.sample) {
      this.virtualFiles.set('sample.obj', SAMPLE_OBJ);
    }
  }

  resolve(meshPath) {
    const normalized = normalizePath(meshPath || '');
    if (!normalized) return null;
    if (this.virtualFiles.has(normalized)) {
      return { path: normalized, virtualText: this.virtualFiles.get(normalized) };
    }

    const withoutPackage = normalized.replace(/^package:\/\//, '');
    const candidates = [];
    if (path.isAbsolute(normalized)) candidates.push(normalized);
    candidates.push(path.resolve(this.urdfDir, normalized));
    candidates.push(path.resolve(this.urdfDir, withoutPackage));
    candidates.push(path.resolve(this.urdfDir, withoutPackage.split('/').slice(1).join('/')));
    for (const assetDir of this.opts.assetDirs) {
      candidates.push(path.resolve(assetDir, normalized));
      candidates.push(path.resolve(assetDir, withoutPackage));
      candidates.push(path.resolve(assetDir, withoutPackage.split('/').slice(1).join('/')));
      candidates.push(path.resolve(assetDir, path.basename(withoutPackage)));
    }
    if (this.opts.packageRoot) {
      candidates.push(path.resolve(this.opts.packageRoot, withoutPackage));
      candidates.push(path.resolve(this.opts.packageRoot, withoutPackage.split('/').slice(1).join('/')));
    }

    for (const candidate of candidates.filter(Boolean)) {
      if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
        assertSafeReadPath(candidate, this.opts, this.urdfDir, 'URDF mesh asset');
        return { path: candidate };
      }
    }
    return null;
  }
}

function normalizePath(value) {
  return value.replace(/\\/g, '/').replace(/^\.?\//, '');
}

function extension(filename) {
  return path.extname((filename || '').split('?')[0].split('#')[0]).toLowerCase();
}

function asFloat32Array(values) {
  return values instanceof Float32Array ? values : new Float32Array(values || []);
}

function asInt32Array(values) {
  return values instanceof Int32Array ? values : new Int32Array(values || []);
}

// Mesh geometry is marshalled to the WASM binding as binary typed arrays via
// setVisualMesh/setCollisionMesh (referenced by `meshRef` in the JSON payload),
// not inlined as JSON number arrays. This keeps the payload string tiny and
// avoids V8's max-string-length ceiling on mesh-dense models. Populated by
// makeGeometryPayload, registered in main() before createURDFPhysicsScene.
const meshBuffers = new Map(); // meshRef -> { positions, normals, uvs, indices }
let meshRefCounter = 0;

function makeGeometryPayload(mesh, matrix, name) {
  const geom = mesh.geometry;
  const pos = geom?.getAttribute('position');
  if (!pos || pos.count < 3) return null;
  const normal = geom.getAttribute('normal');
  const uv = geom.getAttribute('uv');
  const index = geom.getIndex();
  const meshRef = `mesh_${meshRefCounter++}`;
  meshBuffers.set(meshRef, {
    positions: asFloat32Array(pos.array),
    normals: normal ? asFloat32Array(normal.array) : new Float32Array(),
    uvs: uv ? asFloat32Array(uv.array) : new Float32Array(),
    indices: index ? asInt32Array(index.array) : new Int32Array()
  });
  return {
    name,
    matrix: matrixToUSDArray(matrix),
    meshRef
  };
}

function collectMeshPayloads(root, baseMatrix, fallbackName) {
  const payloads = [];
  root.updateMatrixWorld(true);
  let index = 0;
  root.traverse((obj) => {
    if (!obj.isMesh) return;
    const matrix = new THREE.Matrix4().copy(baseMatrix).multiply(obj.matrixWorld);
    const payload = makeGeometryPayload(obj, matrix, obj.name || `${fallbackName}_${index}`);
    if (payload) {
      payload.name = payloads.length ? `${payload.name}_${payloads.length}` : payload.name;
      payloads.push(payload);
    }
    index++;
  });
  return payloads;
}

function primitiveObjectFromGeometry(geometry, name) {
  const mesh = new THREE.Mesh(geometry);
  mesh.name = name;
  const group = new THREE.Group();
  group.add(mesh);
  return group;
}

// Merge every sub-mesh of `object` (e.g. an OBJ split into many `o`/`g`
// objects, as in ms_human_700's Rib1L.obj) into a single mesh. A MuJoCo
// <mesh> is one mesh regardless of how the source file is grouped, matching
// the native loader which merges all `v`/`f` records.
function flattenToSingleMesh(object, name) {
  object.updateMatrixWorld(true);
  const positions = [];
  const normals = [];
  const uvs = [];
  const indices = [];
  let base = 0;
  let hasNormals = true;
  let hasUVs = true;
  const v = new THREE.Vector3();
  const nrm = new THREE.Vector3();
  object.traverse((obj) => {
    if (!obj.isMesh || !obj.geometry) return;
    const g = obj.geometry;
    const pos = g.getAttribute('position');
    if (!pos) return;
    const nAttr = g.getAttribute('normal');
    const uvAttr = g.getAttribute('uv');
    const idx = g.getIndex();
    const m = obj.matrixWorld;
    const normalMat = new THREE.Matrix3().getNormalMatrix(m);
    for (let i = 0; i < pos.count; i++) {
      v.set(pos.getX(i), pos.getY(i), pos.getZ(i)).applyMatrix4(m);
      positions.push(v.x, v.y, v.z);
      if (nAttr) {
        nrm.set(nAttr.getX(i), nAttr.getY(i), nAttr.getZ(i)).applyMatrix3(normalMat).normalize();
        normals.push(nrm.x, nrm.y, nrm.z);
      } else {
        hasNormals = false;
      }
      if (uvAttr) uvs.push(uvAttr.getX(i), uvAttr.getY(i)); else hasUVs = false;
    }
    if (idx) {
      for (let i = 0; i < idx.count; i++) indices.push(base + idx.getX(i));
    } else {
      for (let i = 0; i < pos.count; i++) indices.push(base + i);
    }
    base += pos.count;
  });
  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
  if (hasNormals && normals.length) geom.setAttribute('normal', new THREE.Float32BufferAttribute(normals, 3));
  if (hasUVs && uvs.length) geom.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
  geom.setIndex(indices);
  // Loaders return per-corner vertices (OBJ); share corners that agree on
  // position, normal and uv so the exported mesh is indexed.
  return primitiveObjectFromGeometry(weldMeshGeometry(geom, { mode: 'keep' }), name);
}

async function loadOBJObject(resolvedAsset) {
  const text = resolvedAsset.virtualText ?? fs.readFileSync(resolvedAsset.path, 'utf8');
  return new OBJLoader().parse(text);
}

async function loadSTLObject(resolvedAsset) {
  const data = fs.readFileSync(resolvedAsset.path);
  const arrayBuffer = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
  // STL is a triangle soup: weld repeated positions and use MuJoCo-style
  // vertex normals, as the native converter and MuJoCo itself do.
  const welded = weldMeshGeometry(new STLLoader().parse(arrayBuffer),
    { mode: 'stl', smoothnormal: !!resolvedAsset.smoothnormal });
  return primitiveObjectFromGeometry(welded, path.basename(resolvedAsset.path, path.extname(resolvedAsset.path)));
}

let usdMeshLoader = null;

async function ensureUSDMeshLoader() {
  if (!usdMeshLoader) {
    usdMeshLoader = new LightUSDLoader();
    await usdMeshLoader.init({ useZstdCompressedWasm: false, useMemory64: false });
    LightUSDLoaderUtils.setLightUSD(usdMeshLoader.native_);
  }
  return usdMeshLoader;
}

async function loadUSDObject(resolvedAsset) {
  const loader = await ensureUSDMeshLoader();
  const data = fs.readFileSync(resolvedAsset.path);
  const sceneData = await parseUSDSceneFromArrayBuffer(
    loader,
    data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength),
    path.basename(resolvedAsset.path)
  );
  const rootNode = sceneData.getDefaultRootNode();
  const defaultMaterial = LightUSDLoaderUtils.createDefaultMaterial();
  return LightUSDLoaderUtils.buildThreeNode(rootNode, defaultMaterial, sceneData, {
    overrideMaterial: false
  });
}

async function parseUSDSceneFromArrayBuffer(loader, arrayBuffer, filename) {
  return new Promise((resolve, reject) => {
    loader.parse(new Uint8Array(arrayBuffer), filename, resolve, reject);
  });
}

async function meshPayloadsForGeometry(geometryEl, originMatrix, resolver, fallbackName, opts) {
  const meshEl = firstElement(geometryEl.body, 'mesh');
  if (meshEl) {
    const filename = meshEl.attrs.filename || meshEl.attrs.url || '';
    const resolved = resolver.resolve(filename);
    if (!resolved) {
      if (opts.allowMissing) {
        console.warn(`Skipping missing mesh: ${filename}`);
        return [];
      }
      throw new Error(`Mesh asset not found: ${filename}`);
    }

    const scale = parseNumbers(meshEl.attrs.scale, [1, 1, 1]);
    const meshMatrix = new THREE.Matrix4()
      .copy(originMatrix)
      .multiply(new THREE.Matrix4().makeScale(scale[0] || 1, scale[1] || 1, scale[2] || 1));
    const ext = extension(resolved.path || filename);
    let object = null;
    if (ext === '.obj') {
      object = await loadOBJObject(resolved);
    } else if (ext === '.stl') {
      object = await loadSTLObject(resolved);
    } else if (USD_MESH_EXTENSIONS.has(ext)) {
      object = await loadUSDObject(resolved);
    } else if (opts.allowMissing) {
      console.warn(`Skipping unsupported mesh extension ${ext || '(none)'}: ${filename}`);
      return [];
    } else {
      throw new Error(`Unsupported mesh extension ${ext || '(none)'}: ${filename}`);
    }
    return collectMeshPayloads(object, meshMatrix, fallbackName);
  }

  const boxEl = firstElement(geometryEl.body, 'box');
  if (boxEl) {
    const size = parseNumbers(boxEl.attrs.size, [1, 1, 1]);
    return collectMeshPayloads(
      primitiveObjectFromGeometry(new THREE.BoxGeometry(size[0] || 1, size[1] || 1, size[2] || 1), fallbackName),
      originMatrix,
      fallbackName
    );
  }

  const sphereEl = firstElement(geometryEl.body, 'sphere');
  if (sphereEl) {
    const radius = numberAttr(sphereEl.attrs, 'radius', 0.5);
    return collectMeshPayloads(
      primitiveObjectFromGeometry(new THREE.SphereGeometry(radius, 24, 12), fallbackName),
      originMatrix,
      fallbackName
    );
  }

  const cylinderEl = firstElement(geometryEl.body, 'cylinder');
  if (cylinderEl) {
    const radius = numberAttr(cylinderEl.attrs, 'radius', 0.5);
    const length = numberAttr(cylinderEl.attrs, 'length', 1);
    const yToZ = new THREE.Matrix4().makeRotationX(Math.PI / 2);
    return collectMeshPayloads(
      primitiveObjectFromGeometry(new THREE.CylinderGeometry(radius, radius, length, 24, 1), fallbackName),
      new THREE.Matrix4().copy(originMatrix).multiply(yToZ),
      fallbackName
    );
  }

  if (opts.allowMissing) {
    console.warn(`Skipping unsupported geometry block: ${fallbackName}`);
    return [];
  }
  throw new Error(`Unsupported geometry block: ${fallbackName}`);
}

function shapePayloadForUrdfGeometry(geometryEl, originMatrix, fallbackName) {
  const boxEl = firstElement(geometryEl.body, 'box');
  if (boxEl) {
    const size = parseNumbers(boxEl.attrs.size, [1, 1, 1]);
    const half = [(size[0] || 1) * 0.5, (size[1] || 1) * 0.5, (size[2] || 1) * 0.5];
    const matrix = new THREE.Matrix4()
      .copy(originMatrix)
      .multiply(new THREE.Matrix4().makeScale(half[0], half[1], half[2]));
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(matrix),
      shape: { type: 'box' }
    }];
  }

  const sphereEl = firstElement(geometryEl.body, 'sphere');
  if (sphereEl) {
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(originMatrix),
      shape: {
        type: 'sphere',
        radius: numberAttr(sphereEl.attrs, 'radius', 0.5)
      }
    }];
  }

  const cylinderEl = firstElement(geometryEl.body, 'cylinder');
  if (cylinderEl) {
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(originMatrix),
      shape: {
        type: 'cylinder',
        radius: numberAttr(cylinderEl.attrs, 'radius', 0.5),
        height: numberAttr(cylinderEl.attrs, 'length', 1),
        axis: 'Z'
      }
    }];
  }

  return null;
}

function expandMujocoIncludes(xml, baseDir, opts = {}, seen = new Set()) {
  return xml.replace(/<include\b([^>]*?)\/\s*>/gi, (_tag, attrText) => {
    const attrs = parseAttributes(attrText || '');
    if (!attrs.file) return '';
    const includePath = path.resolve(baseDir, attrs.file);
    const safeIncludePath = assertSafeReadPath(includePath, opts, baseDir, 'MJCF include');
    if (seen.has(safeIncludePath)) {
      throw new Error(`Recursive MJCF include: ${safeIncludePath}`);
    }
    seen.add(safeIncludePath);
    const childXML = stripMujocoDocumentRoot(fs.readFileSync(safeIncludePath, 'utf8'));
    seen.delete(safeIncludePath);
    return expandMujocoIncludes(childXML, path.dirname(safeIncludePath), opts, seen);
  });
}

function stripMujocoDocumentRoot(xml) {
  const withoutDecl = xml.replace(/<\?xml[\s\S]*?\?>/i, '');
  const open = withoutDecl.search(/<mujoco\b[^>]*>/i);
  const close = withoutDecl.lastIndexOf('</mujoco>');
  if (open < 0 || close < 0) return withoutDecl;
  const openEnd = withoutDecl.indexOf('>', open);
  return withoutDecl.slice(openEnd + 1, close);
}

// --- MJCF <attach> sub-model composition (mirrors C++ ExpandAttachments) ----
const ATTACH_REF_ATTRS = new Set([
  'name', 'class', 'childclass', 'material', 'mesh', 'hfield', 'texture',
  'joint', 'joint1', 'joint2', 'site', 'site1', 'site2', 'refsite', 'sidesite',
  'tendon', 'tendon1', 'tendon2', 'body', 'body1', 'body2', 'geom', 'geom1',
  'geom2', 'objname']);

function cloneXMLTree(node, parent = null) {
  const c = { name: node.name, attrs: { ...node.attrs }, children: [], parent };
  c.children = node.children.map((ch) => cloneXMLTree(ch, c));
  return c;
}

function prefixAttachTree(node, prefix, radToDeg) {
  for (const k of Object.keys(node.attrs)) {
    if (ATTACH_REF_ATTRS.has(k) && node.attrs[k]) node.attrs[k] = prefix + node.attrs[k];
  }
  if (radToDeg) {
    const kk = 180 / Math.PI;
    if (node.attrs.euler) { const v = parseNumbers(node.attrs.euler, []); if (v.length === 3) node.attrs.euler = v.map((x) => x * kk).join(' '); }
    if (node.attrs.axisangle) { const v = parseNumbers(node.attrs.axisangle, []); if (v.length === 4) { v[3] *= kk; node.attrs.axisangle = v.join(' '); } }
  }
  for (const c of node.children) prefixAttachTree(c, prefix, radToDeg);
}

function findBodyByNameTree(node, name) {
  for (const b of childElements(node, 'body')) {
    if (b.attrs.name === name) return b;
    const r = findBodyByNameTree(b, name);
    if (r) return r;
  }
  return null;
}

function expandMujocoAttachments(root, baseDir, opts = {}) {
  const models = new Map();
  for (const asset of childElements(root, 'asset')) {
    for (const m of childElements(asset, 'model')) {
      if (m.attrs.name && m.attrs.file) models.set(m.attrs.name, m.attrs.file);
    }
  }
  const attaches = [];
  (function find(n) {
    for (const c of n.children) {
      if (c.name === 'attach') attaches.push(c);
      find(c);
    }
  })(root);
  if (!attaches.length) return;

  const parentCompiler = firstChild(root, 'compiler');
  const parentRadian = (parentCompiler?.attrs.angle || 'degree').toLowerCase() === 'radian';

  for (const attach of attaches) {
    const file = models.get(attach.attrs.model);
    if (!file) { console.warn(`MJCF <attach>: unknown model "${attach.attrs.model}"`); continue; }
    const childPath = assertSafeReadPath(path.resolve(baseDir, file), opts, baseDir, 'MJCF attach model');
    const childDir = path.dirname(childPath);
    const childRoot = parseXMLTree(expandMujocoIncludes(fs.readFileSync(childPath, 'utf8'), childDir, opts));
    const cc = firstChild(childRoot, 'compiler');
    const childMeshdir = cc?.attrs.meshdir || cc?.attrs.assetdir || '';
    const childRadian = (cc?.attrs.angle || 'degree').toLowerCase() === 'radian';
    const radToDeg = childRadian && !parentRadian;

    for (const asset of childElements(childRoot, 'asset')) {
      for (const tag of ['mesh', 'hfield', 'skin']) {
        for (const a of childElements(asset, tag)) {
          if (a.attrs.file) a.attrs.file = path.resolve(childDir, childMeshdir, a.attrs.file);
        }
      }
    }
    prefixAttachTree(childRoot, attach.attrs.prefix || '', radToDeg);

    const target = (attach.attrs.prefix || '') + (attach.attrs.body || '');
    let found = null;
    for (const wb of childElements(childRoot, 'worldbody')) {
      found = findBodyByNameTree(wb, target);
      if (found) break;
    }
    if (!found) { console.warn(`MJCF <attach>: body "${attach.attrs.body}" not found in "${attach.attrs.model}"`); continue; }

    const parent = attach.parent;
    const idx = parent.children.indexOf(attach);
    parent.children.splice(idx, 1, cloneXMLTree(found, parent));

    for (const sec of ['default', 'asset', 'tendon', 'actuator', 'sensor', 'equality', 'contact', 'keyframe']) {
      const childSec = firstChild(childRoot, sec);
      if (!childSec) continue;
      let mainSec = firstChild(root, sec);
      if (!mainSec) { mainSec = { name: sec, attrs: {}, children: [], parent: root }; root.children.push(mainSec); }
      for (const c of childElements(childSec)) {
        if (c.name === 'model') continue;
        mainSec.children.push(cloneXMLTree(c, mainSec));
      }
    }
  }
}

// Basename -> path index of mesh files under baseDir. Last-resort fallback for
// scenes whose <mesh file> paths are relative to an included file's directory
// (e.g. ms_human_700: assets/asset/*.xml referencing "../geometry/*.stl").
// Names appearing in more than one place are dropped to avoid ambiguity.
function buildMeshIndex(baseDir) {
  const index = new Map();
  const ambiguous = new Set();
  const walk = (dir) => {
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        walk(full);
      } else if (entry.isFile()) {
        const ext = path.extname(entry.name).toLowerCase();
        if (ext !== '.stl' && ext !== '.obj' && ext !== '.msh') continue;
        const key = entry.name.toLowerCase();
        if (index.has(key)) ambiguous.add(key);
        else index.set(key, full);
      }
    }
  };
  walk(baseDir);
  for (const key of ambiguous) index.delete(key);
  return index;
}

// Resolve a MJCF mesh file, tolerating models that omit <compiler meshdir>
// but keep assets under an "assets/" subdir (e.g. skydio_x2, google_robot).
// Falls back to --asset-dir entries and a recursive basename index; returns the
// primary candidate (which may not exist) when nothing resolves so the caller
// can honor --allow-missing.
function resolveMujocoMeshFile(file, meshBaseDir, baseDir, opts, meshIndex) {
  const candidates = [
    path.resolve(meshBaseDir, file),
    path.resolve(baseDir, file),
    path.resolve(baseDir, 'assets', file),
    path.resolve(baseDir, path.basename(file))
  ];
  for (const dir of (opts?.assetDirs || [])) {
    candidates.push(path.resolve(dir, file));
    candidates.push(path.resolve(dir, path.basename(file)));
  }
  for (const candidate of candidates) {
    if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
      return assertSafeReadPath(candidate, opts, baseDir, 'MJCF mesh asset');
    }
  }
  const indexed = meshIndex?.get(path.basename(file).toLowerCase());
  if (indexed) return assertSafeReadPath(indexed, opts, baseDir, 'MJCF mesh asset');
  return candidates[0];
}

function collectMujocoAssets(root, baseDir, opts) {
  const compiler = firstChild(root, 'compiler');
  // <compiler meshdir> wins; assetdir is the shared meshdir/texturedir default.
  const meshDir = compiler?.attrs.meshdir || compiler?.attrs.assetdir || '';
  const meshBaseDir = path.resolve(baseDir, meshDir);
  const meshIndex = buildMeshIndex(baseDir);
  const meshes = new Map();
  for (const asset of childElements(root, 'asset')) {
    for (const mesh of childElements(asset, 'mesh')) {
      const file = mesh.attrs.file || '';
      if (!file) continue;
      const name = mesh.attrs.name || path.basename(file, path.extname(file));
      meshes.set(name, {
        path: resolveMujocoMeshFile(file, meshBaseDir, baseDir, opts, meshIndex),
        scale: parseNumbers(mesh.attrs.scale, [1, 1, 1]),
        refpos: parseNumbers(mesh.attrs.refpos, [0, 0, 0]),
        refquat: parseNumbers(mesh.attrs.refquat, [1, 0, 0, 0]),
        smoothnormal: mesh.attrs.smoothnormal === 'true'
      });
    }
  }
  return meshes;
}

// Minimal grayscale PNG decoder (node `zlib`), enough for <asset><hfield> maps:
// 8-bit greyscale/RGB/RGBA, non-interlaced. Returns { width, height, gray:
// Uint8Array } or null. Heightfields only need luminance.
function decodePNGGray(buffer) {
  const SIG = [137, 80, 78, 71, 13, 10, 26, 10];
  for (let i = 0; i < 8; i++) if (buffer[i] !== SIG[i]) return null;
  let pos = 8, width = 0, height = 0, bitDepth = 0, colorType = 0;
  const idat = [];
  while (pos < buffer.length) {
    const len = buffer.readUInt32BE(pos); pos += 4;
    const type = buffer.toString('ascii', pos, pos + 4); pos += 4;
    if (type === 'IHDR') {
      width = buffer.readUInt32BE(pos);
      height = buffer.readUInt32BE(pos + 4);
      bitDepth = buffer[pos + 8];
      colorType = buffer[pos + 9];
      const interlace = buffer[pos + 12];
      if (bitDepth !== 8 || interlace !== 0) return null;  // keep it simple
    } else if (type === 'IDAT') {
      idat.push(buffer.subarray(pos, pos + len));
    } else if (type === 'IEND') {
      break;
    }
    pos += len + 4;  // skip data + CRC
  }
  if (!width || !height) return null;
  const channels = colorType === 0 ? 1 : colorType === 2 ? 3 : colorType === 6 ? 4 : 0;
  if (!channels) return null;
  let raw;
  try {
    raw = zlib.inflateSync(Buffer.concat(idat));
  } catch (e) {
    return null;
  }
  const stride = width * channels;
  if (raw.length < (stride + 1) * height) return null;
  // Un-filter scanlines in place into `out` (the 5 PNG filter types).
  const out = Buffer.alloc(stride * height);
  const paeth = (a, b, c) => {
    const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
  };
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const inOff = y * (stride + 1) + 1;
    const outOff = y * stride;
    for (let x = 0; x < stride; x++) {
      const rawv = raw[inOff + x];
      const a = x >= channels ? out[outOff + x - channels] : 0;
      const b = y > 0 ? out[outOff - stride + x] : 0;
      const c = (x >= channels && y > 0) ? out[outOff - stride + x - channels] : 0;
      let v;
      switch (filter) {
        case 0: v = rawv; break;
        case 1: v = rawv + a; break;
        case 2: v = rawv + b; break;
        case 3: v = rawv + ((a + b) >> 1); break;
        case 4: v = rawv + paeth(a, b, c); break;
        default: return null;
      }
      out[outOff + x] = v & 0xff;
    }
  }
  // Collapse to luminance.
  const gray = new Uint8Array(width * height);
  for (let i = 0; i < width * height; i++) {
    if (channels === 1) gray[i] = out[i];
    else gray[i] = Math.round(0.299 * out[i * channels] + 0.587 * out[i * channels + 1] + 0.114 * out[i * channels + 2]);
  }
  return { width, height, gray };
}

// Collect <asset><hfield> declarations. File-based fields decode a PNG (rows
// reversed: MuJoCo stores row 0 at the -y edge); inline fields read
// nrow/ncol/elevation. All normalized to [0,1]. Mirrors the C++ parser.
function collectMujocoHFields(root, baseDir, opts) {
  const compiler = firstChild(root, 'compiler');
  const meshDir = compiler?.attrs.meshdir || compiler?.attrs.assetdir || '';
  const meshBaseDir = path.resolve(baseDir, meshDir);
  const meshIndex = buildMeshIndex(baseDir);
  const hfields = new Map();
  for (const asset of childElements(root, 'asset')) {
    for (const hf of childElements(asset, 'hfield')) {
      const file = hf.attrs.file || '';
      let name = hf.attrs.name || (file ? path.basename(file, path.extname(file)) : '');
      if (!name) continue;
      const size = parseNumbers(hf.attrs.size, [1, 1, 1, 0.1]);
      let nrow = 0, ncol = 0, data = null;
      if (file) {
        const fpath = resolveMujocoMeshFile(file, meshBaseDir, baseDir, opts, meshIndex);
        if (fs.existsSync(fpath)) {
          const png = decodePNGGray(fs.readFileSync(fpath));
          if (png) {
            ncol = png.width; nrow = png.height;
            data = new Float32Array(nrow * ncol);
            for (let r = 0; r < nrow; r++) {
              for (let c = 0; c < ncol; c++) {
                // reverse rows: data row 0 is the bottom (-y) image row
                data[r * ncol + c] = png.gray[c + (nrow - 1 - r) * ncol];
              }
            }
          }
        }
        if (!data && !opts.allowMissing) {
          throw new Error(`MJCF hfield file not found/decodable: ${file}`);
        }
      } else {
        nrow = Math.trunc(numberAttr(hf.attrs, 'nrow', 0));
        ncol = Math.trunc(numberAttr(hf.attrs, 'ncol', 0));
        const elev = parseNumbers(hf.attrs.elevation, []);
        if (nrow > 0 && ncol > 0 && elev.length === nrow * ncol) {
          data = Float32Array.from(elev);
        }
      }
      if (nrow > 1 && ncol > 1 && data) {
        // normalize to [0,1] as MuJoCo's compiler does
        let emin = Infinity, emax = -Infinity;
        for (const v of data) { if (v < emin) emin = v; if (v > emax) emax = v; }
        const range = emax - emin;
        for (let i = 0; i < data.length; i++) {
          data[i] -= emin;
          if (range > 1e-9) data[i] /= range;
        }
        hfields.set(name, { nrow, ncol, size, data });
      }
    }
  }
  return hfields;
}

// Tessellate a heightfield's top surface into a THREE.BufferGeometry in the
// field's local frame (x in [-rx,rx] over ncol, y in [-ry,ry] over nrow,
// z = normalized * elevation_z), with smooth per-vertex normals. Matches the
// C++ TessellateHField layout exactly.
function buildHFieldGeometry(hf) {
  const nr = hf.nrow, nc = hf.ncol;
  const rx = hf.size[0], ry = hf.size[1], ez = hf.size[2] ?? 1;
  const dx = (2 * rx) / (nc - 1), dy = (2 * ry) / (nr - 1);
  const H = (r, c) => {
    r = Math.max(0, Math.min(nr - 1, r));
    c = Math.max(0, Math.min(nc - 1, c));
    return hf.data[r * nc + c] * ez;
  };
  const positions = new Float32Array(nr * nc * 3);
  const normals = new Float32Array(nr * nc * 3);
  for (let r = 0; r < nr; r++) {
    for (let c = 0; c < nc; c++) {
      const i = (r * nc + c) * 3;
      positions[i] = -rx + c * dx;
      positions[i + 1] = -ry + r * dy;
      positions[i + 2] = H(r, c);
      // Gradient over the ACTUAL neighbor span so the border ring is a correct
      // one-sided difference (1 cell) instead of a halved central difference.
      const cl = Math.max(0, c - 1), cr = Math.min(nc - 1, c + 1);
      const rb = Math.max(0, r - 1), rt = Math.min(nr - 1, r + 1);
      const hx = (H(r, cr) - H(r, cl)) / ((cr - cl) * dx);
      const hy = (H(rt, c) - H(rb, c)) / ((rt - rb) * dy);
      let nx = -hx, ny = -hy, nz = 1;
      const len = Math.hypot(nx, ny, nz) || 1;
      normals[i] = nx / len; normals[i + 1] = ny / len; normals[i + 2] = nz / len;
    }
  }
  const indices = new Uint32Array((nr - 1) * (nc - 1) * 6);
  let k = 0;
  for (let r = 0; r < nr - 1; r++) {
    for (let c = 0; c < nc - 1; c++) {
      const v00 = r * nc + c, v01 = r * nc + (c + 1);
      const v10 = (r + 1) * nc + c, v11 = (r + 1) * nc + (c + 1);
      indices[k++] = v00; indices[k++] = v01; indices[k++] = v11;
      indices[k++] = v00; indices[k++] = v11; indices[k++] = v10;
    }
  }
  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  geom.setAttribute('normal', new THREE.BufferAttribute(normals, 3));
  geom.setIndex(new THREE.BufferAttribute(indices, 1));
  return geom;
}

function mujocoJointType(type) {
  if (type === 'hinge') return 'revolute';
  if (type === 'slide') return 'prismatic';
  if (type === 'free') return 'floating';
  // ball (3-DOF rotation) -> USD PhysicsSphericalJoint (matches web/js/urdf.js).
  if (type === 'ball') return 'spherical';
  return 'fixed';
}

function parseMujocoInertial(bodyNode) {
  const inertialNode = firstChild(bodyNode, 'inertial');
  if (!inertialNode) return {};
  const full = parseNumbers(inertialNode.attrs.fullinertia, []);
  const inertial = {
    mass: numberAttr(inertialNode.attrs, 'mass', 0),
    centerOfMass: parseNumbers(inertialNode.attrs.pos, [0, 0, 0])
  };
  if (full.length >= 6) {
    // Carry all 6 components [Ixx Iyy Izz Ixy Ixz Iyz] so the shared C++
    // converter diagonalizes them into diagonalInertia + principalAxes. Keep a
    // diagonal fallback for consumers that ignore fullInertia.
    inertial.fullInertia = full.slice(0, 6);
    inertial.diagonalInertia = [full[0], full[1], full[2]];
  } else if (full.length >= 3) {
    inertial.diagonalInertia = [full[0], full[1], full[2]];
  } else {
    // `diaginertia` is "Ixx Iyy Izz" — parse all three (numberAttr only reads a
    // single Number(), which yields NaN for the multi-value string and silently
    // dropped the inertia). Mirrors web/js/urdf.js::parseMujocoInertial.
    inertial.diagonalInertia = parseNumbers(inertialNode.attrs.diaginertia, []);
  }
  return inertial;
}

function shapePayloadForMujocoGeom(geomNode, originMatrix, fallbackName, bodyWorld = new THREE.Matrix4()) {
  const geomType = geomNode.attrs.type || (geomNode.attrs.mesh ? 'mesh' : 'sphere');
  if (geomType === 'mesh') return null;

  const size = parseNumbers(geomNode.attrs.size, []);
  if (geomType === 'box') {
    const half = [size[0] || 1, size[1] || 1, size[2] || 1];
    const matrix = new THREE.Matrix4()
      .copy(originMatrix)
      .multiply(new THREE.Matrix4().makeScale(half[0], half[1], half[2]));
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(matrix),
      shape: { type: 'box' }
    }];
  }

  if (geomType === 'sphere') {
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(originMatrix),
      shape: {
        type: 'sphere',
        radius: size[0] || 0.5
      }
    }];
  }

  if (geomType === 'ellipsoid') {
    const radii = [size[0] || 0.5, size[1] || 0.5, size[2] || 0.5];
    const matrix = new THREE.Matrix4()
      .copy(originMatrix)
      .multiply(new THREE.Matrix4().makeScale(radii[0], radii[1], radii[2]));
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(matrix),
      shape: {
        type: 'sphere',
        radius: 1
      }
    }];
  }

  if (geomType === 'cylinder' || geomType === 'capsule') {
    const fromto = parseNumbers(geomNode.attrs.fromto, []);
    let matrix = new THREE.Matrix4().copy(originMatrix);
    let height = size[1] ? size[1] * 2 : 1;
    if (fromto.length >= 6) {
      // fromto spans p1->p2 (pos/quat ignored): center at the midpoint and
      // orient the cylinder's Z axis along the segment.
      const p1 = new THREE.Vector3(fromto[0], fromto[1], fromto[2]);
      const p2 = new THREE.Vector3(fromto[3], fromto[4], fromto[5]);
      const dir = p2.clone().sub(p1);
      height = dir.length() || 1;
      const center = p1.clone().add(p2).multiplyScalar(0.5);
      const quatZ = new THREE.Quaternion().setFromUnitVectors(
        new THREE.Vector3(0, 0, 1), dir.clone().normalize());
      // fromto replaces the geom's pose but is still placed in body world space.
      matrix = new THREE.Matrix4().copy(bodyWorld).multiply(
        new THREE.Matrix4().compose(center, quatZ, new THREE.Vector3(1, 1, 1)));
    }
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(matrix),
      shape: {
        type: geomType,
        radius: size[0] || 0.5,
        height,
        axis: 'Z'
      }
    }];
  }

  if (geomType === 'plane') {
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(originMatrix),
      shape: {
        type: 'plane',
        width: (size[0] && size[0] > 0 ? size[0] : 1) * 2,
        length: (size[1] && size[1] > 0 ? size[1] : 1) * 2,
        axis: 'Z'
      }
    }];
  }

  return null;
}

function addMujocoPhysicsAttrs(payload, geomNode) {
  if (!payload || !geomNode?.attrs) return payload;
  const group = numberAttr(geomNode.attrs, 'group');
  if (Number.isFinite(group)) payload.group = group;
  const condim = numberAttr(geomNode.attrs, 'condim');
  if (Number.isFinite(condim)) payload.condim = condim;
  const margin = numberAttr(geomNode.attrs, 'margin');
  const solmix = numberAttr(geomNode.attrs, 'solmix');
  if (Number.isFinite(margin) || Number.isFinite(solmix)) {
    payload.mjc = payload.mjc || {};
    if (Number.isFinite(margin)) payload.mjc.margin = margin;
    if (Number.isFinite(solmix)) payload.mjc.solmix = solmix;
  }
  return payload;
}

function buildMujocoActuators(root) {
  const actuators = [];
  for (const actuatorRoot of childElements(root, 'actuator')) {
    for (const actNode of childElements(actuatorRoot)) {
      const type = actNode.name;
      const joint = actNode.attrs.joint || '';
      const name = actNode.attrs.name || `${actNode.name}_${joint || actuators.length}`;
      // Mirror the C++ parser's exclusivity: muscle/general/plugin/adhesion/
      // cylinder/intvelocity/damper (and any tendon/site/body target) go to the
      // MjcActuator path (parseMujocoMuscleActuators), not the Newton PD path.
      const isMjc = type === 'muscle' || type === 'general' || type === 'adhesion'
        || type === 'cylinder' || type === 'intvelocity' || type === 'damper'
        || type === 'plugin' || actNode.attrs.tendon || actNode.attrs.site
        || actNode.attrs.body;
      if (isMjc) continue;
      if (!joint) {
        console.warn(`MJCF actuator "${name}" has no joint target; it is not converted to USD.`);
        continue;
      }
      const act = {
        name,
        joint,
        control: 'pd'
      };
      const kp = numberAttr(actNode.attrs, 'kp');
      if (Number.isFinite(kp)) {
        act.kp = kp;
      } else {
        const gain = parseNumbers(actNode.attrs.gainprm, []);
        if (gain.length) act.kp = gain[0];
      }
      const kd = numberAttr(actNode.attrs, 'kv');
      if (Number.isFinite(kd)) {
        act.kd = kd;
      } else {
        const bias = parseNumbers(actNode.attrs.biasprm, []);
        if (bias.length >= 3) act.kd = Math.abs(bias[2]);
      }
      const forceRange = parseNumbers(actNode.attrs.forcerange, []);
      if (forceRange.length >= 2) {
        act.maxEffort = Math.max(Math.abs(forceRange[0]), Math.abs(forceRange[1]));
      }
      const ctrlRange = parseNumbers(actNode.attrs.ctrlrange, []);
      if (actNode.name === 'motor' && ctrlRange.length >= 2) {
        act.constEffort = Math.max(Math.abs(ctrlRange[0]), Math.abs(ctrlRange[1]));
      }
      const delay = numberAttr(actNode.attrs, 'delay');
      if (Number.isFinite(delay)) act.delaySteps = Math.max(1, Math.round(delay));
      actuators.push(act);
    }
  }
  return actuators;
}

// MJCF <tendon> -> tendon payload entries (consumed by the C++ converter's
// AddMjcTendonFromJson). Preserve solver, limit, visual, and routing metadata
// so the USD MuJoCo extension can round-trip richer fixed/spatial tendons.
function parseMujocoTendons(root) {
  const tendonRoot = firstChild(root, 'tendon');
  if (!tendonRoot) return [];
  const out = [];
  for (const node of childElements(tendonRoot)) {
    if (node.name !== 'fixed' && node.name !== 'spatial') continue;
    const range = parseNumbers(node.attrs.range, []);
    const frcRange = parseNumbers(node.attrs.actuatorfrcrange, []);
    const tendon = { name: node.attrs.name || `tendon_${out.length}`, type: node.name };
    if (node.name === 'fixed') {
      const jlist = [];
      for (const j of childElements(node, 'joint')) {
        if (!j.attrs.joint) continue;
        jlist.push({ joint: j.attrs.joint, coef: numberAttr(j.attrs, 'coef', 1) });
      }
      if (!jlist.length) continue;
      tendon.joints = jlist;
    } else {
      // Spatial (muscle) tendon: ordered <site>/<geom sidesite=..> waypoints.
      const path = [];
      const sideSites = [];
      for (const w of childElements(node)) {
        if (w.name === 'site' && w.attrs.site) path.push({ site: w.attrs.site });
        else if (w.name === 'geom') {
          const wp = { geom: w.attrs.geom || '' };
          if (w.attrs.sidesite) {
            wp.sidesite = w.attrs.sidesite;
            sideSites.push({ site: w.attrs.sidesite });
          }
          path.push(wp);
        } else if (w.name === 'pulley') {
          const divisor = numberAttr(w.attrs, 'divisor');
          if (Number.isFinite(divisor)) {
            if (!tendon.routeDivisors) tendon.routeDivisors = [];
            tendon.routeDivisors.push(divisor);
          }
        }
      }
      if (!path.length) continue;
      tendon.path = path;
      if (sideSites.length) tendon.sideSites = sideSites;
      if (node.attrs.width) tendon.width = numberAttr(node.attrs, 'width');
      const rgba = parseNumbers(node.attrs.rgba, []);
      if (rgba.length >= 4) tendon.rgba = rgba.slice(0, 4);
    }
    if (Number.isFinite(numberAttr(node.attrs, 'stiffness'))) tendon.stiffness = numberAttr(node.attrs, 'stiffness');
    if (Number.isFinite(numberAttr(node.attrs, 'damping'))) tendon.damping = numberAttr(node.attrs, 'damping');
    if (Number.isFinite(numberAttr(node.attrs, 'armature'))) tendon.armature = numberAttr(node.attrs, 'armature');
    if (Number.isFinite(numberAttr(node.attrs, 'frictionloss'))) tendon.frictionloss = numberAttr(node.attrs, 'frictionloss');
    if (Number.isFinite(numberAttr(node.attrs, 'margin'))) tendon.margin = numberAttr(node.attrs, 'margin');
    if (node.attrs.group !== undefined) tendon.group = Math.round(numberAttr(node.attrs, 'group'));
    if (node.attrs.limited !== undefined) tendon.limited = node.attrs.limited;
    if (node.attrs.actuatorfrclimited !== undefined) tendon.actuatorfrclimited = node.attrs.actuatorfrclimited;
    if (range.length >= 2) tendon.range = [range[0], range[1]];
    if (frcRange.length >= 2) tendon.actuatorfrcrange = [frcRange[0], frcRange[1]];
    for (const [attr, key] of [
      ['springlength', 'springlength'],
      ['solreflimit', 'solreflimit'],
      ['solimplimit', 'solimplimit'],
      ['solreffriction', 'solreffriction'],
      ['solimpfriction', 'solimpfriction']
    ]) {
      const values = parseNumbers(node.attrs[attr], []);
      if (values.length) tendon[key] = values;
    }
    out.push(tendon);
  }
  return out;
}

// MJCF <site> in bodies -> site payload entries with a baked world transform
// (the routing points for spatial/muscle tendons). Recurses the body tree.
function collectMujocoSites(worldbody) {
  const out = [];
  const visit = (bodyNode, parentWorld) => {
    const bodyWorld = new THREE.Matrix4().copy(parentWorld).multiply(matrixFromPoseAttrs(bodyNode.attrs));
    for (const s of childElements(bodyNode, 'site')) {
      if (!s.attrs.name) continue;
      const siteWorld = new THREE.Matrix4().copy(bodyWorld).multiply(matrixFromPoseAttrs(s.attrs));
      const site = { name: s.attrs.name, matrix: matrixToUSDArray(siteWorld) };
      if (s.attrs.group !== undefined) site.group = parseInt(s.attrs.group, 10) || 0;
      const size = parseNumbers(s.attrs.size, []);
      if (size.length) site.size = size[0];
      out.push(site);
    }
    for (const child of childElements(bodyNode, 'body')) visit(child, bodyWorld);
  };
  for (const body of childElements(worldbody, 'body')) visit(body, new THREE.Matrix4());
  return out;
}

// MJCF <general>/<muscle> (and tendon/site-targeted) actuators -> MjcActuator
// payload entries, preserving the MuJoCo gain/bias/lengthrange parameters.
function parseMujocoMuscleActuators(root) {
  const actRoot = firstChild(root, 'actuator');
  if (!actRoot) return [];
  const out = [];
  for (const node of childElements(actRoot)) {
    const type = node.name;
    const tendon = node.attrs.tendon || '';
    const site = node.attrs.site || '';
    const joint = node.attrs.joint || '';
    const body = node.attrs.body || '';
    const isMjc = type === 'muscle' || type === 'general' || type === 'adhesion'
      || type === 'cylinder' || type === 'intvelocity' || type === 'damper'
      || type === 'plugin' || tendon || site || body;
    if (!isMjc) continue;
    const a = {
      name: node.attrs.name || `${type}_${joint || tendon || site || body}`,
      actuatorType: type
    };
    if (joint) a.targetJoint = joint;
    if (tendon) a.targetTendon = tendon;
    if (site) a.targetSite = site;
    if (body) a.targetBody = body;
    const gainprm = parseNumbers(node.attrs.gainprm, []);
    if (gainprm.length) a.gainPrm = gainprm;
    else if (node.attrs.gain !== undefined) a.gainPrm = [numberAttr(node.attrs, 'gain')];
    const biasprm = parseNumbers(node.attrs.biasprm, []);
    if (biasprm.length) a.biasPrm = biasprm;
    const dynprm = parseNumbers(node.attrs.dynprm, []);
    if (dynprm.length) a.dynPrm = dynprm;
    const lengthrange = parseNumbers(node.attrs.lengthrange, []);
    if (lengthrange.length >= 2) a.lengthRange = [lengthrange[0], lengthrange[1]];
    const ctrlrange = parseNumbers(node.attrs.ctrlrange, []);
    if (ctrlrange.length >= 2) a.ctrlRange = [ctrlrange[0], ctrlrange[1]];
    const forcerange = parseNumbers(node.attrs.forcerange, []);
    if (forcerange.length >= 2) a.forceRange = [forcerange[0], forcerange[1]];
    const actrange = parseNumbers(node.attrs.actrange, []);
    if (actrange.length >= 2) a.actRange = [actrange[0], actrange[1]];
    const gear = parseNumbers(node.attrs.gear, []);
    if (gear.length) a.gear = gear;
    if (node.attrs.gaintype) a.gainType = node.attrs.gaintype;
    if (node.attrs.biastype) a.biasType = node.attrs.biastype;
    if (node.attrs.dyntype) a.dynType = node.attrs.dyntype;
    if (node.attrs.ctrllimited) a.ctrlLimited = node.attrs.ctrllimited;
    if (node.attrs.forcelimited) a.forceLimited = node.attrs.forcelimited;
    if (node.attrs.actlimited) a.actLimited = node.attrs.actlimited;
    if (node.attrs.group !== undefined) a.group = Math.round(numberAttr(node.attrs, 'group'));
    if (node.attrs.actdim !== undefined) a.actDim = Math.round(numberAttr(node.attrs, 'actdim'));
    if (node.attrs.cranklength !== undefined) a.crankLength = numberAttr(node.attrs, 'cranklength');
    if (node.attrs.inheritrange !== undefined) a.inheritRange = numberAttr(node.attrs, 'inheritrange');
    if (node.attrs.jointinparent !== undefined) a.jointInParent = mjcBoolAttr(node.attrs.jointinparent);
    if (node.attrs.actearly !== undefined) a.actEarly = mjcBoolAttr(node.attrs.actearly);
    if (node.attrs.refsite) a.refSite = node.attrs.refsite;
    if (node.attrs.slidersite) a.sliderSite = node.attrs.slidersite;
    // Engine-plugin actuator (<plugin plugin=".." instance="..">).
    if (node.attrs.plugin) a.plugin = node.attrs.plugin;
    if (node.attrs.instance) a.instance = node.attrs.instance;
    out.push(a);
  }
  return out;
}

// <extension><plugin plugin="..."><instance name="..."><config key= value=>
// -> plugin-instance declarations (referenced by <actuator><plugin instance=..>).
function parseMujocoPlugins(root) {
  const ext = firstChild(root, 'extension');
  if (!ext) return [];
  const out = [];
  for (const pluginNode of childElements(ext, 'plugin')) {
    const pluginId = pluginNode.attrs.plugin || '';
    for (const inst of childElements(pluginNode, 'instance')) {
      const instName = inst.attrs.name || '';
      if (!instName) continue;
      const p = { instance: instName };
      if (pluginId) p.plugin = pluginId;
      const config = {};
      for (const cfg of childElements(inst, 'config')) {
        const key = cfg.attrs.key || '';
        if (!key) continue;
        config[key] = cfg.attrs.value || '';
      }
      if (Object.keys(config).length) p.config = config;
      out.push(p);
    }
  }
  return out;
}

// MJCF <custom><numeric|text> -> { numeric:[{name,data}], text:[{name,data}] }.
function parseMujocoCustom(root) {
  const customRoot = firstChild(root, 'custom');
  if (!customRoot) return {};
  const out = {};
  const numeric = [];
  for (const n of childElements(customRoot, 'numeric')) {
    if (!n.attrs.name) continue;
    numeric.push({ name: n.attrs.name, data: parseNumbers(n.attrs.data, []) });
  }
  const text = [];
  for (const t of childElements(customRoot, 'text')) {
    if (!t.attrs.name) continue;
    text.push({ name: t.attrs.name, data: t.attrs.data || '' });
  }
  if (numeric.length) out.numeric = numeric;
  if (text.length) out.text = text;
  return out;
}

// MJCF <sensor> children -> sensor payload entries (-> MjcSensor prims).
function parseMujocoSensors(root) {
  const sensorRoot = firstChild(root, 'sensor');
  if (!sensorRoot) return [];
  const out = [];
  for (const node of childElements(sensorRoot)) {
    const type = node.name;
    if (!type) continue;
    const s = { type, name: node.attrs.name || `${type}_${out.length}` };
    const targetAliases = ['site', 'joint', 'actuator', 'tendon', 'body', 'xbody',
      'geom', 'camera', 'light', 'sensor', 'numeric', 'text', 'tuple', 'key',
      'plugin'];
    if (node.attrs.objtype !== undefined) {
      s.objtype = node.attrs.objtype;
      if (node.attrs.objname !== undefined) s.objname = node.attrs.objname;
    } else {
      for (const k of targetAliases) {
        if (node.attrs[k] !== undefined) { s.objtype = k; s.objname = node.attrs[k]; break; }
      }
    }
    if (node.attrs.reftype !== undefined) {
      s.reftype = node.attrs.reftype;
      if (node.attrs.refname !== undefined) s.refname = node.attrs.refname;
    } else {
      for (const k of targetAliases) {
        const refKey = `ref${k}`;
        const camelRefKey = `ref${k[0].toUpperCase()}${k.slice(1)}`;
        if (node.attrs[refKey] !== undefined) {
          s.reftype = k;
          s.refname = node.attrs[refKey];
          break;
        }
        if (node.attrs[camelRefKey] !== undefined) {
          s.reftype = k;
          s.refname = node.attrs[camelRefKey];
          break;
        }
      }
    }
    if (node.attrs.group !== undefined) s.group = Math.round(numberAttr(node.attrs, 'group'));
    if (node.attrs.cutoff !== undefined) s.cutoff = numberAttr(node.attrs, 'cutoff');
    if (node.attrs.noise !== undefined) s.noise = numberAttr(node.attrs, 'noise');
    const user = parseNumbers(node.attrs.user, []);
    if (user.length) s.user = user;
    out.push(s);
  }
  return out;
}

// MJCF <asset><material> -> material payload entries (-> UsdShade Material).
function parseMujocoMaterials(root, baseDir = '', opts = {}) {
  const asset = firstChild(root, 'asset');
  if (!asset) return [];
  // Resolve <texture name file=...> to an absolute path so a material's texture
  // becomes a UsdUVTexture (builtin/file-less textures are skipped).
  const compiler = firstChild(root, 'compiler');
  const texDir = compiler?.attrs.texturedir || compiler?.attrs.assetdir || '';
  const texBaseDir = path.resolve(baseDir, texDir);
  const meshIndex = buildMeshIndex(baseDir);
  const texFiles = new Map();
  for (const t of childElements(asset, 'texture')) {
    if (t.attrs.name && t.attrs.file) {
      texFiles.set(t.attrs.name, resolveMujocoMeshFile(t.attrs.file, texBaseDir, baseDir, opts, meshIndex));
    }
  }
  const out = [];
  for (const m of childElements(asset, 'material')) {
    if (!m.attrs.name) continue;
    const mat = { name: m.attrs.name };
    const rgba = parseNumbers(m.attrs.rgba, []);
    if (rgba.length >= 4) mat.rgba = rgba.slice(0, 4);
    for (const k of ['metallic', 'roughness', 'specular', 'emission', 'reflectance']) {
      if (m.attrs[k] !== undefined) mat[k] = numberAttr(m.attrs, k);
    }
    if (m.attrs.texture && texFiles.has(m.attrs.texture)) {
      // Emit a portable, source-relative reference (e.g. "assets/foo.png") for
      // inputs:file so the exported usda/usdc opens beside its assets; keep the
      // absolute source in textureAbs (ignored by the converter) so usdz export
      // can embed the bytes.
      const abs = texFiles.get(m.attrs.texture);
      let rel = baseDir ? path.relative(baseDir, abs) : path.basename(abs);
      if (!rel || rel.startsWith('..')) rel = path.basename(abs);
      mat.texture = rel.split(path.sep).join('/');
      mat.textureAbs = abs;
    }
    out.push(mat);
  }
  return out;
}

// MJCF <keyframe><key qpos/qvel/act/ctrl/mpos/mquat> -> keyframe payload entries.
function parseMujocoKeyframes(root) {
  const kfRoot = firstChild(root, 'keyframe');
  if (!kfRoot) return [];
  const out = [];
  for (const key of childElements(kfRoot, 'key')) {
    const k = { name: key.attrs.name || `key_${out.length}` };
    for (const a of ['qpos', 'qvel', 'act', 'ctrl', 'mpos', 'mquat']) {
      const v = parseNumbers(key.attrs[a], []);
      if (v.length) k[a] = v;
    }
    out.push(k);
  }
  return out;
}

// MuJoCo boolean/flag attr: <flag x="enable|disable">, <compiler x="true|false">.
function mjcBoolAttr(v) { return v === 'true' || v === 'enable' || v === '1'; }

// MJCF <light>/<camera> (in worldbody or nested bodies) -> payload lights/cameras
// with a baked world matrix, consumed by the C++ converter's AddLightFromJson /
// AddCameraFromJson (UsdLux + GeomCamera).
function collectMujocoLightsCameras(worldbody) {
  const lights = [];
  const cameras = [];
  const visit = (node, world) => {
    for (const l of childElements(node, 'light')) {
      const m = new THREE.Matrix4().copy(world).multiply(matrixFromPoseAttrs(l.attrs));
      const type = l.attrs.type
        || (mjcBoolAttr(l.attrs.directional || 'false') ? 'directional' : 'spot');
      const light = { name: l.attrs.name || 'light', type, matrix: matrixToUSDArray(m) };
      light.dir = parseNumbers(l.attrs.dir, [0, 0, -1]).slice(0, 3);
      const diffuse = parseNumbers(l.attrs.diffuse, []);
      if (diffuse.length >= 3) light.color = diffuse.slice(0, 3);
      if (l.attrs.castshadow !== undefined) light.castshadow = mjcBoolAttr(l.attrs.castshadow);
      if (l.attrs.cutoff !== undefined) light.cutoff = numberAttr(l.attrs, 'cutoff');
      lights.push(light);
    }
    for (const c of childElements(node, 'camera')) {
      const m = new THREE.Matrix4().copy(world).multiply(matrixFromPoseAttrs(c.attrs));
      const cam = { name: c.attrs.name || 'camera', matrix: matrixToUSDArray(m) };
      if (c.attrs.fovy !== undefined) cam.fovy = numberAttr(c.attrs, 'fovy');
      if (c.attrs.projection === 'orthographic'
          || (c.attrs.orthographic !== undefined && mjcBoolAttr(c.attrs.orthographic))) {
        cam.orthographic = true;
      }
      cameras.push(cam);
    }
    for (const b of childElements(node, 'body')) {
      visit(b, new THREE.Matrix4().copy(world).multiply(matrixFromPoseAttrs(b.attrs)));
    }
  };
  visit(worldbody, new THREE.Matrix4());
  return { lights, cameras };
}

// MJCF <option>/<option><flag>/<compiler> -> payload.mjcScene {option,flag,compiler}
// consumed by the C++ converter's ApplyMjcSceneOptions -> MjcSceneAPI.
function parseMujocoSceneOptions(root) {
  const ms = {};
  const option = firstChild(root, 'option');
  if (option) {
    const opt = {};
    const has = (k) => option.attrs[k] !== undefined;
    const num = (k) => { if (has(k)) opt[k] = numberAttr(option.attrs, k); };
    const inum = (k) => { if (has(k)) opt[k] = Math.round(numberAttr(option.attrs, k)); };
    const tok = (k) => { if (has(k)) opt[k] = option.attrs[k]; };
    const vec = (k) => { const v = parseNumbers(option.attrs[k], []); if (v.length) opt[k] = v; };
    ['timestep', 'impratio', 'density', 'viscosity', 'o_margin', 'tolerance',
     'ls_tolerance', 'noslip_tolerance', 'ccd_tolerance'].forEach(num);
    ['iterations', 'ls_iterations', 'noslip_iterations', 'ccd_iterations',
     'sdf_iterations', 'sdf_initpoints'].forEach(inum);
    ['integrator', 'cone', 'jacobian', 'solver'].forEach(tok);
    ['wind', 'magnetic', 'o_solref', 'o_solimp', 'o_friction'].forEach(vec);
    if (Object.keys(opt).length) ms.option = opt;
    const flag = firstChild(option, 'flag');
    if (flag) {
      const fl = {};
      ['constraint', 'equality', 'frictionloss', 'limit', 'contact', 'gravity',
       'clampctrl', 'warmstart', 'filterparent', 'actuation', 'refsafe', 'sensor',
       'midphase', 'nativeccd', 'eulerdamp', 'autoreset', 'island', 'override',
       'energy', 'fwdinv', 'invdiscrete', 'multiccd'].forEach((k) => {
        if (flag.attrs[k] !== undefined) fl[k] = mjcBoolAttr(flag.attrs[k]);
      });
      if (Object.keys(fl).length) ms.flag = fl;
    }
  }
  const compiler = firstChild(root, 'compiler');
  if (compiler) {
    const comp = {};
    const has = (k) => compiler.attrs[k] !== undefined;
    const num = (k) => { if (has(k)) comp[k] = numberAttr(compiler.attrs, k); };
    const tok = (k) => { if (has(k)) comp[k] = compiler.attrs[k]; };
    const boolean = (k) => { if (has(k)) comp[k] = mjcBoolAttr(compiler.attrs[k]); };
    boolean('autolimits'); num('boundmass'); num('boundinertia'); num('settotalmass');
    boolean('usethread'); boolean('balanceinertia'); tok('angle'); boolean('fitaabb');
    boolean('fusestatic'); tok('inertiafromgeom'); boolean('alignfree'); boolean('saveinertial');
    const igr = parseNumbers(compiler.attrs.inertiagrouprange, []);
    if (igr.length >= 2) {
      comp.inertiagrouprange_min = Math.round(igr[0]);
      comp.inertiagrouprange_max = Math.round(igr[1]);
    }
    if (Object.keys(comp).length) ms.compiler = comp;
  }
  return ms;
}

// MJCF <equality> connect/weld/joint -> equality payload entries.
function parseMujocoEqualities(root) {
  const eqRoot = firstChild(root, 'equality');
  if (!eqRoot) return [];
  const out = [];
  for (const node of childElements(eqRoot)) {
    const kind = node.name;
    if (!['connect', 'weld', 'joint'].includes(kind)) continue;
    const eq = { name: node.attrs.name || `${kind}_${out.length}`, type: kind };
    if (kind === 'joint') {
      eq.joint1 = node.attrs.joint1 || '';
      if (node.attrs.joint2) eq.joint2 = node.attrs.joint2;
      const poly = parseNumbers(node.attrs.polycoef, []);
      if (poly.length) eq.polycoef = poly;
    } else {
      eq.body1 = node.attrs.body1 || '';
      if (node.attrs.body2) eq.body2 = node.attrs.body2;
      const anchor = parseNumbers(node.attrs.anchor, []);
      if (anchor.length) eq.anchor = anchor;
      if (kind === 'weld' && Number.isFinite(numberAttr(node.attrs, 'torquescale'))) {
        eq.torquescale = numberAttr(node.attrs, 'torquescale');
      }
    }
    const solref = parseNumbers(node.attrs.solref, []);
    if (solref.length) eq.solref = solref;
    const solimp = parseNumbers(node.attrs.solimp, []);
    if (solimp.length) eq.solimp = solimp;
    out.push(eq);
  }
  return out;
}

// MJCF <contact><exclude body1 body2> -> filteredPairs entries.
function parseMujocoContactExcludes(root) {
  const contactRoot = firstChild(root, 'contact');
  if (!contactRoot) return [];
  const out = [];
  for (const node of childElements(contactRoot, 'exclude')) {
    if (!node.attrs.body1 || !node.attrs.body2) continue;
    out.push({ body1: node.attrs.body1, body2: node.attrs.body2 });
  }
  return out;
}

// MJCF <contact><pair geom1 geom2 ...> -> contactPairs entries.
function parseMujocoContactPairs(root) {
  const contactRoot = firstChild(root, 'contact');
  if (!contactRoot) return [];
  const out = [];
  for (const node of childElements(contactRoot, 'pair')) {
    if (!node.attrs.geom1 || !node.attrs.geom2) continue;
    const pr = { name: node.attrs.name || `pair_${out.length}`, geom1: node.attrs.geom1, geom2: node.attrs.geom2 };
    if (node.attrs.condim !== undefined) pr.condim = Math.round(numberAttr(node.attrs, 'condim'));
    if (node.attrs.margin !== undefined) pr.margin = numberAttr(node.attrs, 'margin');
    if (node.attrs.gap !== undefined) pr.gap = numberAttr(node.attrs, 'gap');
    const friction = parseNumbers(node.attrs.friction, []);
    if (friction.length) pr.friction = friction;
    const solref = parseNumbers(node.attrs.solref, []);
    if (solref.length) pr.solref = solref;
    const solimp = parseNumbers(node.attrs.solimp, []);
    if (solimp.length) pr.solimp = solimp;
    out.push(pr);
  }
  return out;
}

async function mujocoGeomPayloads(geomNode, meshAssets, hfields, fallbackName, opts, bodyWorld = new THREE.Matrix4()) {
  const geomType = geomNode.attrs.type || (geomNode.attrs.mesh ? 'mesh' : 'sphere');
  // Bake the body-chain world transform into the geom matrix: the USD converter
  // places every link Xform at identity, so each geom carries its full world
  // placement (body_world * geom-local pose).
  const originMatrix = new THREE.Matrix4().copy(bodyWorld).multiply(matrixFromPoseAttrs(geomNode.attrs));
  if (geomType === 'hfield') {
    const hf = hfields.get(geomNode.attrs.hfield);
    if (!hf) {
      if (opts.allowMissing) {
        console.warn(`Skipping missing MJCF hfield asset: ${geomNode.attrs.hfield}`);
        return [];
      }
      throw new Error(`MJCF hfield asset not found: ${geomNode.attrs.hfield}`);
    }
    const mesh = new THREE.Mesh(buildHFieldGeometry(hf));
    mesh.name = fallbackName;
    return collectMeshPayloads(mesh, originMatrix, fallbackName);
  }
  if (geomType === 'mesh') {
    const meshName = geomNode.attrs.mesh;
    const meshAsset = meshAssets.get(meshName);
    if (!meshAsset) {
      if (opts.allowMissing) {
        console.warn(`Skipping missing MJCF mesh asset: ${meshName}`);
        return [];
      }
      throw new Error(`MJCF mesh asset not found: ${meshName}`);
    }
    if (!fs.existsSync(meshAsset.path)) {
      if (opts.allowMissing) {
        console.warn(`Skipping MJCF mesh file not found on disk: ${meshAsset.path}`);
        return [];
      }
      throw new Error(`MJCF mesh file not found: ${meshAsset.path}`);
    }
    const ext = extension(meshAsset.path);
    let object = null;
    if (ext === '.stl') {
      object = await loadSTLObject(meshAsset);
    } else if (ext === '.obj') {
      object = await loadOBJObject(meshAsset);
    } else if (USD_MESH_EXTENSIONS.has(ext)) {
      object = await loadUSDObject(meshAsset);
    } else if (opts.allowMissing) {
      console.warn(`Skipping unsupported MJCF mesh extension ${ext || '(none)'}: ${meshAsset.path}`);
      return [];
    } else {
      throw new Error(`Unsupported MJCF mesh extension ${ext || '(none)'}: ${meshAsset.path}`);
    }
    const scale = parseNumbers(geomNode.attrs.scale, meshAsset.scale || [1, 1, 1]);
    // MuJoCo mesh refpos/refquat: translate vertices by -refpos and rotate by
    // the conjugate of refquat before placement (e.g. shadow_dexee fingers).
    const rq = meshAsset.refquat || [1, 0, 0, 0];
    const rp = meshAsset.refpos || [0, 0, 0];
    const refQuat = new THREE.Quaternion(rq[1] || 0, rq[2] || 0, rq[3] || 0, rq[0] ?? 1).normalize().conjugate();
    const meshMatrix = new THREE.Matrix4()
      .copy(originMatrix)
      .multiply(new THREE.Matrix4().makeRotationFromQuaternion(refQuat))
      .multiply(new THREE.Matrix4().makeTranslation(-(rp[0] || 0), -(rp[1] || 0), -(rp[2] || 0)))
      .multiply(new THREE.Matrix4().makeScale(scale[0] || 1, scale[1] || 1, scale[2] || 1));
    // A MuJoCo <mesh> is a single mesh: merge any multi-object source file
    // (e.g. ms_human_700 Rib1L.obj's 12 `o` groups) into one, matching the
    // native loader and MuJoCo semantics.
    return collectMeshPayloads(flattenToSingleMesh(object, fallbackName), meshMatrix, fallbackName);
  }

  const size = parseNumbers(geomNode.attrs.size, []);
  if (geomType === 'box') {
    return collectMeshPayloads(
      primitiveObjectFromGeometry(new THREE.BoxGeometry((size[0] || 1) * 2, (size[1] || 1) * 2, (size[2] || 1) * 2), fallbackName),
      originMatrix,
      fallbackName
    );
  }
  if (geomType === 'sphere') {
    return collectMeshPayloads(
      primitiveObjectFromGeometry(new THREE.SphereGeometry(size[0] || 0.5, 24, 12), fallbackName),
      originMatrix,
      fallbackName
    );
  }
  if (geomType === 'ellipsoid') {
    const radii = [size[0] || 0.5, size[1] || 0.5, size[2] || 0.5];
    const matrix = new THREE.Matrix4()
      .copy(originMatrix)
      .multiply(new THREE.Matrix4().makeScale(radii[0], radii[1], radii[2]));
    return collectMeshPayloads(
      primitiveObjectFromGeometry(new THREE.SphereGeometry(1, 24, 12), fallbackName),
      matrix,
      fallbackName
    );
  }
  if (geomType === 'cylinder' || geomType === 'capsule') {
    const radius = size[0] || 0.5;
    const length = size[1] ? size[1] * 2 : 1;
    const yToZ = new THREE.Matrix4().makeRotationX(Math.PI / 2);
    return collectMeshPayloads(
      primitiveObjectFromGeometry(new THREE.CylinderGeometry(radius, radius, length, 24, 1), fallbackName),
      new THREE.Matrix4().copy(originMatrix).multiply(yToZ),
      fallbackName
    );
  }
  if (geomType === 'plane') {
    // A finite display quad for the (infinite) MuJoCo ground plane, baked as a
    // thin box like the C++ parser (size = half-x, half-y, grid-spacing).
    const sx = size[0] > 0 ? size[0] : 1;
    const sy = size[1] > 0 ? size[1] : 1;
    const sz = size[2] > 0 ? size[2] : 0.001;
    return collectMeshPayloads(
      primitiveObjectFromGeometry(new THREE.BoxGeometry(sx * 2, sy * 2, sz * 2), fallbackName),
      originMatrix,
      fallbackName
    );
  }

  if (opts.allowMissing) {
    console.warn(`Skipping unsupported MJCF geom type: ${geomType}`);
    return [];
  }
  throw new Error(`Unsupported MJCF geom type: ${geomType}`);
}

// Resolve MuJoCo <default> class inheritance into per-class attribute tables for
// geoms and joints. Nested <default class="..."> inherit their parent default's
// merged attrs; the unnamed top-level <default> is the root baseline. This lets
// us recover attributes (group/contype/type/...) that real menagerie models set
// via classes/childclass rather than per-element, e.g. visual geoms tagged only
// through `childclass="robot"` + `<default class="robot"><geom group="2"/>`.
function parseMujocoDefaults(root) {
  const geom = new Map();
  const joint = new Map();
  let rootGeom = {};
  let rootJoint = {};
  function walk(defNode, inheritedGeom, inheritedJoint) {
    const gEl = firstChild(defNode, 'geom');
    const myGeom = { ...inheritedGeom, ...(gEl ? gEl.attrs : {}) };
    const jEl = firstChild(defNode, 'joint');
    const myJoint = { ...inheritedJoint, ...(jEl ? jEl.attrs : {}) };
    const cls = defNode.attrs.class;
    if (cls) {
      geom.set(cls, myGeom);
      joint.set(cls, myJoint);
    } else {
      rootGeom = myGeom;
      rootJoint = myJoint;
    }
    for (const child of childElements(defNode, 'default')) {
      walk(child, myGeom, myJoint);
    }
  }
  for (const defNode of childElements(root, 'default')) {
    walk(defNode, {}, {});
  }
  return { geom, joint, rootGeom, rootJoint };
}

// Effective attrs = class/childclass defaults merged under the element's own attrs.
function resolveElementAttrs(node, classTable, rootAttrs, childclass) {
  const cls = node.attrs.class || childclass;
  const base = cls && classTable.has(cls) ? classTable.get(cls) : rootAttrs;
  return { ...base, ...node.attrs };
}

async function buildMujocoPayload(xmlText, opts, baseDir) {
  opts = optsWithDefaultReadRoot(opts, baseDir);
  const expanded = expandMujocoIncludes(xmlText, baseDir, opts);
  const root = parseXMLTree(expanded);
  // Graft <attach> sub-models before semantic parsing.
  expandMujocoAttachments(root, baseDir, opts);
  if (root.name !== 'mujoco') {
    throw new Error('Expected <mujoco> root for MJCF input.');
  }

  // Honor <compiler angle="..." eulerseq="..."> for all orientation specifiers.
  const compilerEl = firstChild(root, 'compiler');
  const angleAttr = (compilerEl?.attrs?.angle || 'degree').toLowerCase();
  mjcfPoseCtx = {
    toRad: angleAttr === 'radian' ? 1 : Math.PI / 180,
    eulerseq: compilerEl?.attrs?.eulerseq || 'xyz'
  };

  // Dissolve <frame> grouping transforms (needs the angle ctx above).
  for (const worldbody of childElements(root, 'worldbody')) flattenMujocoFramesTree(worldbody);

  const defaults = parseMujocoDefaults(root);
  const meshAssets = collectMujocoAssets(root, baseDir, opts);
  const hfields = collectMujocoHFields(root, baseDir, opts);
  // MuJoCo merges every <worldbody> block (the local one plus any pulled in via
  // <include>) into a single world; visit them all, not just the first.
  const worldbodies = childElements(root, 'worldbody');
  if (worldbodies.length === 0) {
    throw new Error('MJCF input has no <worldbody>.');
  }

  const links = [];
  const joints = [];
  const actuators = buildMujocoActuators(root);
  const tendons = parseMujocoTendons(root);
  const equalities = parseMujocoEqualities(root);
  const filteredPairs = parseMujocoContactExcludes(root);
  const contactPairs = parseMujocoContactPairs(root);
  const custom = parseMujocoCustom(root);
  const plugins = parseMujocoPlugins(root);
  const sites = [];
  const mjcActuators = parseMujocoMuscleActuators(root);
  const mjcScene = parseMujocoSceneOptions(root);
  const keyframes = parseMujocoKeyframes(root);
  const lights = [];
  const cameras = [];
  for (const worldbody of worldbodies) {
    sites.push(...collectMujocoSites(worldbody));
    const lc = collectMujocoLightsCameras(worldbody);
    lights.push(...lc.lights);
    cameras.push(...lc.cameras);
  }
  const materials = parseMujocoMaterials(root, baseDir, opts);
  const sensors = parseMujocoSensors(root);
  let visualCount = 0;
  let collisionCount = 0;

  // Bake one <geom> into a link payload's visuals/collisions. Shared by the
  // per-body traversal and the worldbody-level (static) geom collection.
  async function appendGeomToLink(rawGeomNode, childclass, linkPayload, bodyWorld, fallbackPrefix, geomIndex, dualCollider = false) {
    // Resolve <default>/childclass inheritance so class-tagged attributes
    // (type/group/contype/...) are visible to classification and tessellation.
    const effAttrs = resolveElementAttrs(rawGeomNode, defaults.geom, defaults.rootGeom, childclass);
    const geomNode = { name: rawGeomNode.name, attrs: effAttrs, children: rawGeomNode.children || [] };
    const geomName = effAttrs.name || effAttrs.mesh || `${fallbackPrefix}_geom_${geomIndex}`;
    // MuJoCo visibility is by geom group: 0-2 visible, 3-5 hidden/collision.
    // The resolved group (explicit on the geom, else from its class) is
    // authoritative and wins over the class NAME — e.g. iit_softfoot tags some
    // class="collision" meshes with group="0" to make them visible. No group =>
    // default group 0 (visible); class name is only a fallback hint.
    const geomClass = (effAttrs.class || '').toLowerCase();
    const geomGroup = Number(effAttrs.group);
    const isVisual = Number.isFinite(geomGroup) ? geomGroup < 3
      : (geomClass.includes('collision') || geomClass.includes('collider')) ? false
      : true;
    const geomWorld = new THREE.Matrix4().copy(bodyWorld).multiply(matrixFromPoseAttrs(geomNode.attrs));
    let payloads = null;
    if (!isVisual && !opts.tessellateCollisionShapes) {
      payloads = shapePayloadForMujocoGeom(geomNode, geomWorld, geomName, bodyWorld);
    }
    if (!payloads) {
      payloads = await mujocoGeomPayloads(geomNode, meshAssets, hfields, geomName, opts, bodyWorld);
    }
    if (isVisual) {
      // A world-fixed geom that is visible AND a collider (MuJoCo default
      // contype=conaffinity=1; non-collider only when BOTH are 0) is ALSO
      // emitted as a USD collider so floors/ground/hfield actually collide.
      // The owning link is static, so use an exact triangle-mesh collider
      // (`none`), not a convex hull that would flatten terrain. Build these from
      // the pristine payloads (sharing meshRef, no buffer duplication) before
      // the visual map mutates them. Robot bodies keep the group-based split.
      if (dualCollider) {
        const contype = effAttrs.contype !== undefined ? Number(effAttrs.contype) : 1;
        const conaffinity = effAttrs.conaffinity !== undefined ? Number(effAttrs.conaffinity) : 1;
        if (contype !== 0 || conaffinity !== 0) {
          for (const payload of payloads) {
            const col = { ...payload, approximation: 'none' };
            addMujocoPhysicsAttrs(col, geomNode);
            linkPayload.collisions.push(col);
            collisionCount += 1;
          }
        }
      }
      linkPayload.visuals.push(...payloads.map((payload) => {
        const p = addMujocoPhysicsAttrs(payload, geomNode);
        if (effAttrs.material) p.material = effAttrs.material;
        return p;
      }));
      visualCount += payloads.length;
    } else {
      // Default approximation `convexHull` matches the convention in
      // `src/tydra/urdf-to-usd.cc::AddCollisionAPIs` and mirrors
      // NVIDIA / Newton's mujoco-usd-converter (see lightgeom
      // `doc/usd.md` "Mesh + collider convention"). Override per-geom
      // with `approximation: "none"` for triangle-soup MJCF colliders.
      for (const payload of payloads) {
        payload.approximation = payload.approximation || 'convexHull';
        addMujocoPhysicsAttrs(payload, geomNode);
      }
      linkPayload.collisions.push(...payloads);
      collisionCount += payloads.length;
    }
  }

  async function visitBody(bodyNode, parentName = '', inheritedChildclass = '', parentWorld = new THREE.Matrix4()) {
    const linkName = bodyNode.attrs.name || `body_${links.length}`;
    // childclass propagates to this body's own geoms/joints and descendants
    // until overridden by a nearer childclass or an explicit element class.
    const childclass = bodyNode.attrs.childclass || inheritedChildclass;
    // Accumulate the body-chain world transform (this body relative to parent,
    // composed onto the parent's world frame).
    const bodyWorld = new THREE.Matrix4().copy(parentWorld).multiply(matrixFromPoseAttrs(bodyNode.attrs));
    // A <freejoint> (or <joint type="free">) -> 6-DOF floating base: not joined
    // to its parent. Marked so the converter emits a free articulation root.
    const hasFree = !!firstChild(bodyNode, 'freejoint')
      || childElements(bodyNode, 'joint').some((j) => j.attrs.type === 'free');
    const linkPayload = {
      name: linkName,
      inertial: parseMujocoInertial(bodyNode),
      visuals: [],
      collisions: []
    };
    if (hasFree) linkPayload.floating = true;
    if (bodyNode.attrs.mocap !== undefined && mjcBoolAttr(bodyNode.attrs.mocap)) {
      linkPayload.mocap = true;
    }

    let geomIndex = 0;
    for (const rawGeomNode of childElements(bodyNode, 'geom')) {
      await appendGeomToLink(rawGeomNode, childclass, linkPayload, bodyWorld, linkName, geomIndex);
      geomIndex++;
    }

    links.push(linkPayload);

    if (parentName && !hasFree) {
      const jointNodes = childElements(bodyNode, 'joint');
      const bodyOrigin = parseNumbers(bodyNode.attrs.pos, [0, 0, 0]);
      const bodyMatrix = matrixToUSDArray(matrixFromPoseAttrs(bodyNode.attrs));
      const identityMatrix = matrixToUSDArray(new THREE.Matrix4());

      // Build one joint entry from a <joint> node connecting prev -> child with
      // the given origin frame.
      const makeJoint = (jointNode, prev, child, origin, originMatrix) => {
        const jAttrs = resolveElementAttrs(jointNode, defaults.joint, defaults.rootJoint, childclass);
        const axis = parseNumbers(jAttrs.axis, [0, 0, 1]);
        const range = parseNumbers(jAttrs.range, []);
        // Revolute limits are radians (the converter re-converts to degrees);
        // MJCF hinge ranges are in the compiler angle unit (degrees by default),
        // slide ranges are meters and pass through.
        const limScale = (jAttrs.type || 'hinge') === 'hinge' ? mjcfPoseCtx.toRad : 1;
        return {
          name: jAttrs.name || `${prev}_to_${child}`,
          type: mujocoJointType(jAttrs.type || 'hinge'),
          parent: prev,
          child,
          axis,
          axisToken: axisToToken(axis),
          origin,
          originMatrix,
          limit: range.length >= 2 ? { lower: range[0] * limScale, upper: range[1] * limScale } : {},
          dynamics: {
            damping: numberAttr(jAttrs, 'damping'),
            friction: numberAttr(jAttrs, 'frictionloss')
          }
        };
      };

      if (jointNodes.length <= 1) {
        if (jointNodes[0]) {
          joints.push(makeJoint(jointNodes[0], parentName, linkName, bodyOrigin, bodyMatrix));
        } else {
          joints.push({
            name: `${parentName}_to_${linkName}_fixed`,
            type: 'fixed',
            parent: parentName,
            child: linkName,
            axis: [1, 0, 0],
            axisToken: 'X',
            origin: bodyOrigin,
            originMatrix: bodyMatrix,
            limit: {},
            dynamics: {}
          });
        }
      } else {
        // MuJoCo allows multiple <joint> per body (composing DOFs). Represent
        // them as a chain of single-DOF joints through (N-1) massless
        // intermediate link Xforms, matching the C++ MJCF parser. Only the
        // first joint carries the body offset; geometry is world-baked on the
        // real body, so the empty intermediates add no visible geometry.
        let prev = parentName;
        for (let k = 0; k < jointNodes.length; k++) {
          const last = k === jointNodes.length - 1;
          const child = last ? linkName : `${linkName}__mjcdof_${k + 1}`;
          if (!last) {
            links.push({ name: child, inertial: { mass: 0 }, visuals: [], collisions: [] });
          }
          joints.push(makeJoint(jointNodes[k], prev, child,
            k === 0 ? bodyOrigin : [0, 0, 0],
            k === 0 ? bodyMatrix : identityMatrix));
          prev = child;
        }
      }
    }

    for (const childBody of childElements(bodyNode, 'body')) {
      await visitBody(childBody, linkName, childclass, bodyWorld);
    }
  }

  for (const worldbody of worldbodies) {
    for (const bodyNode of childElements(worldbody, 'body')) {
      await visitBody(bodyNode);
    }
  }

  // World-fixed geoms living directly under <worldbody> (floor/ground/hfield)
  // belong to no body; collect them onto a single static root link named
  // "world" so they still convert. Mirrors the C++ AddWorldbodyGeomsLink.
  const worldLink = { name: 'world', static: true, inertial: { mass: 0 }, visuals: [], collisions: [] };
  let worldGeomIndex = 0;
  for (const worldbody of worldbodies) {
    for (const rawGeomNode of childElements(worldbody, 'geom')) {
      await appendGeomToLink(rawGeomNode, '', worldLink, new THREE.Matrix4(), 'world', worldGeomIndex, /*dualCollider=*/true);
      worldGeomIndex++;
    }
  }
  if (worldLink.visuals.length || worldLink.collisions.length) {
    links.push(worldLink);
  }

  // Drop Newton joint-actuators whose target joint was not exported (e.g. a root
  // body's joint-to-world, which this converter doesn't emit — iit_softfoot's
  // `position_load` on the `load` slide). The shared converter skips them
  // anyway; filtering keeps the stats and verification honest.
  const exportedJointNames = new Set(joints.map((j) => j.name));
  for (let i = actuators.length - 1; i >= 0; i--) {
    if (actuators[i].joint && !exportedJointNames.has(actuators[i].joint)) {
      console.warn(`MJCF actuator "${actuators[i].name}" targets unexported joint "${actuators[i].joint}"; skipped.`);
      actuators.splice(i, 1);
    }
  }

  const payload = {
    name: root.attrs.model || path.basename(opts.inputFile || 'mujoco_scene', path.extname(opts.inputFile || 'mujoco_scene')),
    upAxis: opts.upAxis,
    sourceFormat: 'mjcf',
    gravity: opts.upAxis === 'Z' ? [0, 0, -1] : [0, -1, 0],
    timestep: numberAttr(firstChild(root, 'option')?.attrs, 'timestep'),
    links,
    joints,
    actuators
  };
  if (tendons.length) payload.tendons = tendons;
  if (equalities.length) payload.equalities = equalities;
  if (filteredPairs.length) payload.filteredPairs = filteredPairs;
  if (contactPairs.length) payload.contactPairs = contactPairs;
  if (Object.keys(custom).length) payload.custom = custom;
  if (plugins.length) payload.plugins = plugins;
  if (sites.length) payload.sites = sites;
  if (mjcActuators.length) payload.mjcActuators = mjcActuators;
  if (Object.keys(mjcScene).length) payload.mjcScene = mjcScene;
  if (keyframes.length) payload.keyframes = keyframes;
  if (lights.length) payload.lights = lights;
  if (cameras.length) payload.cameras = cameras;
  if (materials.length) payload.materials = materials;
  if (sensors.length) payload.sensors = sensors;
  return {
    payload,
    stats: {
      links: links.length,
      joints: joints.length,
      visuals: visualCount,
      collisions: collisionCount,
      actuators: actuators.length,
      tendons: tendons.length,
      equalities: equalities.length,
      sites: sites.length,
      mjcActuators: mjcActuators.length
    }
  };
}

async function buildExportPayload(urdfText, opts, urdfDir) {
  const metadata = parseURDFMetadata(urdfText);
  const resolver = new MeshResolver(opts, urdfDir);
  const links = [];
  let visualCount = 0;
  let collisionCount = 0;

  for (const linkInfo of metadata.links.values()) {
    const linkPayload = {
      name: linkInfo.name,
      inertial: linkInfo.inertial || {},
      visuals: [],
      collisions: []
    };

    let visualIndex = 0;
    for (const visualEl of findElements(linkInfo.body, 'visual')) {
      const geometryEl = firstElement(visualEl.body, 'geometry');
      if (!geometryEl) continue;
      const origin = originToMatrix(firstElement(visualEl.body, 'origin')?.attrs);
      const payloads = await meshPayloadsForGeometry(
        geometryEl,
        origin,
        resolver,
        visualEl.attrs.name || `${linkInfo.name}_visual_${visualIndex}`,
        opts
      );
      linkPayload.visuals.push(...payloads);
      visualCount += payloads.length;
      visualIndex++;
    }

    let collisionIndex = 0;
    for (const collisionEl of findElements(linkInfo.body, 'collision')) {
      const geometryEl = firstElement(collisionEl.body, 'geometry');
      if (!geometryEl) continue;
      const origin = originToMatrix(firstElement(collisionEl.body, 'origin')?.attrs);
      const fallbackName =
        collisionEl.attrs.name || `${linkInfo.name}_collision_${collisionIndex}`;
      let payloads = null;
      if (!opts.tessellateCollisionShapes) {
        payloads = shapePayloadForUrdfGeometry(geometryEl, origin, fallbackName);
      }
      if (!payloads) {
        payloads = await meshPayloadsForGeometry(
          geometryEl,
          origin,
          resolver,
          fallbackName,
          opts
        );
      }
      // URDF <collision> elements default to convex-hull approximation —
      // matches the writer convention in `src/tydra/urdf-to-usd.cc` and
      // mujoco-usd-converter. Override per-geom by authoring
      // `approximation` in the JSON payload.
      for (const payload of payloads) {
        payload.approximation = payload.approximation || 'convexHull';
      }
      linkPayload.collisions.push(...payloads);
      collisionCount += payloads.length;
      collisionIndex++;
    }

    links.push(linkPayload);
  }

  return {
    payload: {
      name: metadata.name || path.basename(opts.inputFile || 'sample', path.extname(opts.inputFile || 'sample')),
      upAxis: opts.upAxis,
      gravity: opts.upAxis === 'Z' ? [0, 0, -1] : [0, -1, 0],
      links,
      joints: metadata.joints,
      actuators: metadata.actuators
    },
    stats: {
      links: links.length,
      joints: metadata.joints.length,
      visuals: visualCount,
      collisions: collisionCount,
      actuators: metadata.actuators.length
    }
  };
}

async function buildPayload(inputText, opts, inputDir) {
  const isMJCF = opts.inputFormat === 'mjcf' ||
    (opts.inputFormat === 'auto' && /<mujoco\b/i.test(inputText));
  if (isMJCF) {
    return buildMujocoPayload(inputText, opts, inputDir);
  }
  return buildExportPayload(inputText, opts, inputDir);
}

function resolveOutputPath(inputFile, format, outputFile) {
  if (outputFile) {
    if (format === 'all') {
      const ext = path.extname(outputFile);
      return ext ? outputFile.slice(0, -ext.length) : outputFile;
    }
    return path.extname(outputFile) ? outputFile : `${outputFile}.${format}`;
  }
  const base = inputFile
    ? inputFile.replace(/\.(urdf|xml)$/i, '')
    : path.resolve(process.cwd(), 'sample-urdf');
  return format === 'all' ? base : `${base}.${format}`;
}

function writeExport(native, format, outPath) {
  if (format === 'usda') {
    const text = native.exportAsUSDA();
    if (!text) throw new Error(native.error() || 'USDA export failed');
    fs.writeFileSync(outPath, text, 'utf8');
    return text.length;
  }
  if (format === 'usdc') {
    const data = native.exportAsUSDC();
    if (!data) throw new Error(native.error() || 'USDC export failed');
    const bytes = new Uint8Array(data);
    fs.writeFileSync(outPath, bytes);
    return bytes.length;
  }
  const data = native.exportAsUSDZ();
  if (!data) throw new Error(native.error() || 'USDZ export failed');
  const bytes = new Uint8Array(data);
  fs.writeFileSync(outPath, bytes);
  return bytes.length;
}

function findDynamicRootLinkCandidates(payload) {
  if (!payload?.links || !Array.isArray(payload.links)) return [];
  const childLinks = new Set();
  const joints = payload.joints;
  if (Array.isArray(joints)) {
    for (const joint of joints) {
      if (joint?.child) childLinks.add(joint.child);
    }
  }
  return payload.links.filter((link) => {
    if (!link?.name) return false;
    if (link.static) return false;
    return !childLinks.has(link.name);
  }).map((link) => link.name);
}

function verifyUSDA(native, stats, payload) {
  const usda = native.exportAsUSDA();
  if (!usda) throw new Error(native.error() || 'USDA verification export failed');
  const required = [
    'PhysicsScene',
    'MjcSceneAPI',
    'NewtonSceneAPI'
  ];
  if (findDynamicRootLinkCandidates(payload).length > 0) {
    required.push('PhysicsRigidBodyAPI');
    required.push('NewtonArticulationRootAPI');
  }
  if (stats.collisions > 0) {
    required.push('PhysicsCollisionAPI');
    required.push('MjcCollisionAPI');
    required.push('NewtonCollisionAPI');
  }
  if (stats.joints > 0) {
    const hasTypedJoint = /\bdef\s+Physics[A-Za-z0-9_]+Joint\b/.test(usda);
    if (!usda.includes('MjcJointAPI') && !hasTypedJoint) {
      required.push('MjcJointAPI');
    }
  }
  if ((stats.actuators || 0) > 0) required.push('NewtonActuator');
  const missing = required.filter((token) => !usda.includes(token));
  if (missing.length) {
    throw new Error(`Verification failed. Missing USDA tokens: ${missing.join(', ')}`);
  }
}

async function main() {
  const opts = parseArgs();
  const inputPath = opts.sample ? null : path.resolve(opts.inputFile);
  const urdfDir = inputPath ? path.dirname(inputPath) : process.cwd();
  const urdfText = opts.sample ? SAMPLE_URDF : fs.readFileSync(inputPath, 'utf8');

  if (inputPath && !opts.assetDirs.length) {
    opts.assetDirs.push(urdfDir);
  }

  const { payload, stats } = await buildPayload(urdfText, opts, urdfDir);
  if (opts.dumpJson) {
    fs.mkdirSync(path.dirname(path.resolve(opts.dumpJson)), { recursive: true });
    fs.writeFileSync(opts.dumpJson, `${JSON.stringify(payload, null, 2)}\n`, 'utf8');
  }

  const lightusd = await LightUSDFactory();
  const native = new lightusd.LightUSDLoaderNative();
  let payloadJSON;
  try {
    payloadJSON = JSON.stringify(payload);
  } catch (err) {
    if (err instanceof RangeError) {
      throw new Error(
        `Payload too large to marshal: tessellated mesh geometry exceeds V8's max string length ` +
        `(~512M chars). Re-run on a lighter model, or reduce mesh density ` +
        `(${stats.visuals} visual meshes, ${stats.collisions} collisions).`
      );
    }
    throw err;
  }

  // Register tessellated mesh geometry as binary typed arrays (referenced by
  // meshRef in the payload) before authoring the scene.
  if (native.clearURDFMeshBuffers) native.clearURDFMeshBuffers();
  for (const [ref, buf] of meshBuffers) {
    if (!native.setVisualMesh(ref, buf.positions, buf.normals, buf.uvs, buf.indices)) {
      throw new Error(native.error() || `Failed to register mesh buffer "${ref}"`);
    }
  }

  const created = native.createURDFPhysicsScene(payloadJSON);
  if (!created) {
    throw new Error(native.error() || 'createURDFPhysicsScene failed');
  }
  const warn = native.warn?.();
  if (warn) console.warn(warn.trim());

  // Raise the USDC writer's conservative WASM size caps when requested, so
  // mesh-dense scenes can export past the 100MB default.
  if ((opts.maxUsdcMb > 0 || opts.maxMemMb > 0) && native.setUSDCExportLimitMB) {
    native.setUSDCExportLimitMB(opts.maxUsdcMb, opts.maxMemMb);
  }

  if (opts.verify) verifyUSDA(native, stats, payload);

  const formats = opts.format === 'all' ? ['usda', 'usdc', 'usdz'] : [opts.format];

  // For usdz output, feed referenced texture bytes into the WASM asset cache so
  // exportAsUSDZ() embeds them (the archive name matches the relative inputs:file
  // reference). usda/usdc reference the same relative path and resolve it on disk.
  if (formats.includes('usdz') && native.setAsset) {
    for (const m of payload.materials || []) {
      if (!m.texture || !m.textureAbs) continue;
      try {
        native.setAsset(m.texture, new Uint8Array(fs.readFileSync(m.textureAbs)));
      } catch (e) {
        console.warn(`texture not embedded into usdz: ${m.textureAbs}: ${e.message}`);
      }
    }
  }

  const basePath = resolveOutputPath(inputPath, opts.format, opts.outputFile);
  for (const format of formats) {
    const outPath = opts.format === 'all' ? `${basePath}.${format}` : basePath;
    fs.mkdirSync(path.dirname(path.resolve(outPath)), { recursive: true });
    const bytes = writeExport(native, format, outPath);
    console.log(`Wrote ${outPath} (${bytes} ${format === 'usda' ? 'chars' : 'bytes'})`);
  }
  native.delete();

  console.log(`Verified ${payload.name}: ${stats.links} links, ${stats.joints} joints, ${stats.visuals} visual meshes, ${stats.collisions} collisions, ${stats.actuators || 0} Newton actuators.`);
  if (opts.verbose) {
    console.log(`Formats: ${formats.join(', ')}`);
    if (opts.dumpJson) console.log(`Payload JSON: ${opts.dumpJson}`);
  }
}

export {
  assertSafeReadPath,
  buildMujocoActuators,
  buildMujocoPayload,
  buildPayload,
  expandMujocoIncludes,
  findDynamicRootLinkCandidates,
  verifyUSDA,
  parseArgs,
  resolveMujocoMeshFile
};

// Run only when invoked directly (not when imported by tests).
//
// Compare REALPATHS. import.meta.url is always symlink-resolved while
// path.resolve(argv[1]) is not, so invoking this file through a symlinked
// parent -- e.g. a checkout reached via ~/work -> /mnt/disk/work, which is
// how many machines lay out a big-disk repo -- made this false. The script
// then did nothing and exited 0: no conversion, no output file, no error.
// In the MJCF roundtrip that surfaced as all 67 models failing with
// "REV-ERROR ... ENOENT", blaming the return trip for a forward step that
// had silently no-op'd.
const isMain = (() => {
  if (!process.argv[1]) return false;
  const self = fileURLToPath(import.meta.url);
  const invoked = path.resolve(process.argv[1]);
  if (self === invoked) return true;
  try {
    return fs.realpathSync(self) === fs.realpathSync(invoked);
  } catch {
    return false;
  }
})();

if (isMain) {
  main().catch((err) => {
    console.error(`urdf-to-usd: ${err.message}`);
    if (process.argv.includes('--verbose') || process.argv.includes('-v')) {
      console.error(err.stack);
    }
    process.exit(1);
  });
}
