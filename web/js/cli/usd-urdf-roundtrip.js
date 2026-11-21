#!/usr/bin/env node
// USD Physics -> URDF JS-only roundtrip tester for LightUSD WASM.

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import LightUSDFactory from '../src/lightusd/lightusd.js';

const RAD_TO_DEG = 180 / Math.PI;
const DEG_TO_RAD = Math.PI / 180;

function printHelp() {
  console.log(`
USD Physics -> URDF JS-only roundtrip tester

Usage:
  node cli/usd-urdf-roundtrip.js [scene.usd|scene.usda|scene.usdc|scene.usdz]
  node cli/usd-urdf-roundtrip.js --sample --format usdc

Options:
  --sample             Generate a USD Physics sample, reload it, and test roundtrip (default without input)
  --format <fmt>       Sample export format: usda or usdc (default: usdc)
  --dump-extracted     Print extracted USD Physics JSON
  --dump-urdf-json     Print converted URDF JSON
  --dump-urdf          Print converted URDF XML
  --no-assert          Convert only; do not fail when expected physics entities are missing
  -v, --verbose        Print summary details
  -h, --help           Show this help
`);
}

function parseArgs(argv = process.argv.slice(2)) {
  const opts = {
    inputFile: null,
    sample: false,
    format: 'usdc',
    dumpExtracted: false,
    dumpUrdfJson: false,
    dumpUrdf: false,
    assert: true,
    verbose: false
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      printHelp();
      process.exit(0);
    } else if (arg === '--sample') {
      opts.sample = true;
    } else if (arg === '--format') {
      opts.format = requireValue(argv, ++i, arg).toLowerCase();
      if (!['usda', 'usdc'].includes(opts.format)) {
        throw new Error('--format must be usda or usdc');
      }
    } else if (arg === '--dump-extracted') {
      opts.dumpExtracted = true;
    } else if (arg === '--dump-urdf-json') {
      opts.dumpUrdfJson = true;
    } else if (arg === '--dump-urdf') {
      opts.dumpUrdf = true;
    } else if (arg === '--no-assert') {
      opts.assert = false;
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

  if (!opts.inputFile) {
    opts.sample = true;
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

function identityMatrix() {
  return [
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
  ];
}

function translationMatrix(x, y, z) {
  const m = identityMatrix();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

function scaleMatrix(x, y, z) {
  const m = identityMatrix();
  m[0] = x;
  m[5] = y;
  m[10] = z;
  return m;
}

function sampleRobotPayload() {
  return {
    name: 'LightUSDRoundtripBot',
    upAxis: 'Z',
    gravity: [0, -1, 0],
    timestep: 0.002,
    links: [
      {
        name: 'base_link',
        inertial: {
          mass: 1.5,
          centerOfMass: [0, 0, 0],
          diagonalInertia: [0.2, 0.25, 0.3]
        },
        visuals: [
          {
            name: 'base_visual',
            matrix: identityMatrix(),
            geometry: {
              positions: [
                -0.2, -0.2, 0,
                 0.2, -0.2, 0,
                 0.0,  0.2, 0
              ],
              normals: [0, 0, 1, 0, 0, 1, 0, 0, 1],
              uvs: [0, 0, 1, 0, 0.5, 1],
              indices: [0, 1, 2]
            }
          }
        ],
        collisions: [
          {
            name: 'base_collision',
            matrix: scaleMatrix(0.5, 0.25, 0.125),
            shape: { type: 'box' }
          }
        ]
      },
      {
        name: 'arm_link',
        inertial: {
          mass: 0.4,
          centerOfMass: [0.25, 0, 0],
          diagonalInertia: [0.05, 0.06, 0.07]
        },
        visuals: [
          {
            name: 'arm_visual',
            matrix: translationMatrix(0.25, 0, 0),
            geometry: {
              positions: [
                0, -0.05, -0.05,
                0.5, -0.05, -0.05,
                0.5,  0.05, -0.05,
                0,  0.05, -0.05
              ],
              indices: [0, 1, 2, 0, 2, 3]
            }
          }
        ],
        collisions: [
          {
            name: 'arm_collision',
            matrix: translationMatrix(0.25, 0, 0),
            shape: { type: 'sphere', radius: 0.12 }
          }
        ]
      }
    ],
    joints: [
      {
        name: 'hinge',
        type: 'revolute',
        parent: 'base_link',
        child: 'arm_link',
        axis: [0, 0, 1],
        axisToken: 'Z',
        originMatrix: translationMatrix(0, 0, 0.2),
        limit: { lower: -1.25, upper: 1.25, effort: 2, velocity: 4 },
        dynamics: { damping: 0.1, friction: 0.02 }
      }
    ]
  };
}

function basenameFromPath(primPath) {
  return String(primPath || '').split('/').filter(Boolean).pop() || '';
}

function parentLinkPath(primPath, linkPaths) {
  let best = null;
  for (const linkPath of linkPaths) {
    if (primPath === linkPath || primPath.startsWith(`${linkPath}/`)) {
      if (!best || linkPath.length > best.length) best = linkPath;
    }
  }
  return best;
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

function geometryToUrdf(geometry = {}, matrix) {
  if (geometry.type === 'box' || geometry.type === 'cube') {
    const scale = matrixScale(matrix);
    // GeomCube emits a scalar size (USD schema); legacy form emits a vec3.
    const base = Array.isArray(geometry.size)
      ? geometry.size
      : (typeof geometry.size === 'number'
          ? [geometry.size, geometry.size, geometry.size]
          : [2, 2, 2]);
    return { type: 'box', size: base.map((v, i) => Number(v || 1) * scale[i]) };
  }
  if (geometry.type === 'sphere') {
    return { type: 'sphere', radius: Number(geometry.radius || 0) };
  }
  if (geometry.type === 'cylinder' || geometry.type === 'capsule') {
    return {
      type: geometry.type,
      radius: Number(geometry.radius || 0),
      length: Number(geometry.length || geometry.height || 0)
    };
  }
  if (geometry.type === 'plane') {
    return {
      type: 'box',
      size: [Number(geometry.width || 1), Number(geometry.length || 1), 0.001]
    };
  }
  return { type: 'mesh', filename: `extracted:${geometry.source || 'mesh'}` };
}

function usdPhysicsToUrdf(extracted) {
  const prims = extracted.prims || [];
  const linkPrims = prims.filter((prim) => hasApi(prim, 'PhysicsRigidBodyAPI'));
  const linkPaths = new Set(linkPrims.map((prim) => prim.path));
  const links = linkPrims.map((prim) => ({
    name: prim.name || basenameFromPath(prim.path),
    path: prim.path,
    inertial: {
      mass: Number(prim.properties?.['physics:mass'] || 0),
      centerOfMass: prim.properties?.['physics:centerOfMass'] || [0, 0, 0],
      diagonalInertia: prim.properties?.['physics:diagonalInertia'] || [0, 0, 0]
    },
    visuals: [],
    collisions: []
  }));
  const linksByPath = new Map(links.map((link) => [link.path, link]));

  for (const prim of prims) {
    if (!prim.geometry) continue;
    const linkPath = parentLinkPath(prim.path, linkPaths);
    if (!linkPath) continue;
    const link = linksByPath.get(linkPath);
    const item = {
      name: prim.name || basenameFromPath(prim.path),
      origin: matrixTranslation(prim.matrix),
      geometry: geometryToUrdf(prim.geometry, prim.matrix)
    };
    if (hasApi(prim, 'PhysicsCollisionAPI')) {
      item.usdPhysics = {
        collisionEnabled: prim.properties?.['physics:collisionEnabled'] !== false,
        meshApproximation: prim.properties?.['physics:approximation'] || null
      };
      item.mujoco = {
        group: prim.properties?.['mjc:group'],
        condim: prim.properties?.['mjc:condim'],
        solmix: prim.properties?.['mjc:solmix'],
        margin: prim.properties?.['mjc:margin']
      };
      if (hasApi(prim, 'NewtonCollisionAPI')) {
        item.newton = {
          contactMargin: prim.properties?.['newton:contactMargin'],
          contactGap: prim.properties?.['newton:contactGap'],
          maxHullVertices: prim.properties?.['newton:maxHullVertices']
        };
      }
      link.collisions.push(item);
    } else {
      item.mujoco = { group: prim.properties?.['mjc:group'] };
      link.visuals.push(item);
    }
  }

  const joints = prims
    .filter((prim) => /^Physics(?:Revolute|Prismatic|Fixed|Joint)/.test(prim.type))
    .map((prim) => {
      const type = prim.type === 'PhysicsRevoluteJoint'
        ? 'revolute'
        : prim.type === 'PhysicsPrismaticJoint'
          ? 'prismatic'
          : 'fixed';
      const parentPath = firstRelTarget(prim, 'physics:body0');
      const childPath = firstRelTarget(prim, 'physics:body1');
      const parent = linksByPath.get(parentPath)?.name || basenameFromPath(parentPath);
      const child = linksByPath.get(childPath)?.name || basenameFromPath(childPath);
      const axisToken = prim.properties?.['physics:axis'] || 'X';
      const lower = prim.properties?.['physics:lowerLimit'];
      const upper = prim.properties?.['physics:upperLimit'];
      const item = {
        name: prim.name || basenameFromPath(prim.path),
        type,
        parent,
        child,
        origin: prim.properties?.['physics:localPos0'] || [0, 0, 0],
        axis: axisToken === 'Y' ? [0, 1, 0] : axisToken === 'Z' ? [0, 0, 1] : [1, 0, 0],
        limit: lower !== undefined || upper !== undefined
          ? {
              lower: type === 'revolute' ? Number(lower || 0) * DEG_TO_RAD : Number(lower || 0),
              upper: type === 'revolute' ? Number(upper || 0) * DEG_TO_RAD : Number(upper || 0)
            }
          : null,
        dynamics: {
          damping: prim.properties?.['mjc:damping'],
          friction: prim.properties?.['mjc:frictionloss']
        },
        usdPhysics: {
          jointEnabled: prim.properties?.['physics:jointEnabled'],
          collisionEnabled: prim.properties?.['physics:collisionEnabled']
        },
        mujoco: {
          group: prim.properties?.['mjc:group']
        }
      };
      if (hasApi(prim, 'NewtonMimicAPI')) {
        item.newton = {
          mimicEnabled: prim.properties?.['newton:mimicEnabled'],
          mimicJoint: firstRelTarget(prim, 'newton:mimicJoint'),
          mimicCoef0: prim.properties?.['newton:mimicCoef0'],
          mimicCoef1: prim.properties?.['newton:mimicCoef1']
        };
      }
      return item;
    });

  return {
    name: 'ConvertedFromUSDPhysics',
    upAxis: extracted.upAxis || 'Y',
    links,
    joints
  };
}

function fmtNumber(value) {
  if (!Number.isFinite(value)) return '0';
  return Number(value).toPrecision(9).replace(/\.?0+$/u, '');
}

function vec(value, fallback = [0, 0, 0]) {
  const a = Array.isArray(value) ? value : fallback;
  return [a[0] || 0, a[1] || 0, a[2] || 0].map(fmtNumber).join(' ');
}

function geometryXML(geometry = {}) {
  if (geometry.type === 'box' || geometry.type === 'cube') {
    // GeomCube emits a scalar size (USD schema); legacy form emits a vec3.
    const size = Array.isArray(geometry.size)
      ? geometry.size
      : (typeof geometry.size === 'number'
          ? [geometry.size, geometry.size, geometry.size]
          : [1, 1, 1]);
    return `<box size="${vec(size, [1, 1, 1])}"/>`;
  }
  if (geometry.type === 'sphere') return `<sphere radius="${fmtNumber(geometry.radius || 0)}"/>`;
  if (geometry.type === 'cylinder') {
    return `<cylinder radius="${fmtNumber(geometry.radius || 0)}" length="${fmtNumber(geometry.length || 0)}"/>`;
  }
  return `<mesh filename="${escapeXML(geometry.filename || 'extracted:mesh.obj')}"/>`;
}

function escapeXML(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/"/g, '&quot;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

function urdfToXML(robot) {
  const lines = [`<robot name="${escapeXML(robot.name)}">`];
  for (const link of robot.links) {
    lines.push(`  <link name="${escapeXML(link.name)}">`);
    if (link.inertial && link.inertial.mass > 0) {
      const i = link.inertial.diagonalInertia || [0, 0, 0];
      lines.push('    <inertial>');
      lines.push(`      <origin xyz="${vec(link.inertial.centerOfMass)}" rpy="0 0 0"/>`);
      lines.push(`      <mass value="${fmtNumber(link.inertial.mass)}"/>`);
      lines.push(`      <inertia ixx="${fmtNumber(i[0] || 0)}" ixy="0" ixz="0" iyy="${fmtNumber(i[1] || 0)}" iyz="0" izz="${fmtNumber(i[2] || 0)}"/>`);
      lines.push('    </inertial>');
    }
    for (const visual of link.visuals) {
      lines.push(`    <visual name="${escapeXML(visual.name)}">`);
      lines.push(`      <origin xyz="${vec(visual.origin)}" rpy="0 0 0"/>`);
      lines.push(`      <geometry>${geometryXML(visual.geometry)}</geometry>`);
      lines.push('    </visual>');
    }
    for (const collision of link.collisions) {
      lines.push(`    <collision name="${escapeXML(collision.name)}">`);
      lines.push(`      <origin xyz="${vec(collision.origin)}" rpy="0 0 0"/>`);
      lines.push(`      <geometry>${geometryXML(collision.geometry)}</geometry>`);
      lines.push('    </collision>');
    }
    lines.push('  </link>');
  }
  for (const joint of robot.joints) {
    lines.push(`  <joint name="${escapeXML(joint.name)}" type="${joint.type}">`);
    lines.push(`    <parent link="${escapeXML(joint.parent)}"/>`);
    lines.push(`    <child link="${escapeXML(joint.child)}"/>`);
    lines.push(`    <origin xyz="${vec(joint.origin)}" rpy="0 0 0"/>`);
    lines.push(`    <axis xyz="${vec(joint.axis, [1, 0, 0])}"/>`);
    if (joint.limit) {
      lines.push(`    <limit lower="${fmtNumber(joint.limit.lower)}" upper="${fmtNumber(joint.limit.upper)}" effort="0" velocity="0"/>`);
    }
    if (joint.dynamics?.damping !== undefined || joint.dynamics?.friction !== undefined) {
      lines.push(`    <dynamics damping="${fmtNumber(joint.dynamics.damping || 0)}" friction="${fmtNumber(joint.dynamics.friction || 0)}"/>`);
    }
    lines.push('  </joint>');
  }
  lines.push('</robot>');
  return `${lines.join('\n')}\n`;
}

function assertRoundtrip(extracted, urdf, expected = null) {
  const prims = extracted.prims || [];
  const apis = new Set(prims.flatMap((prim) => prim.apiSchemas || []));
  const hasSchema = (api) => [...apis].some((name) => name === api || name.startsWith(`${api}:`));
  for (const api of ['PhysicsRigidBodyAPI', 'PhysicsMassAPI']) {
    if (!hasSchema(api)) {
      throw new Error(`Missing extracted API schema: ${api}`);
    }
  }

  if (!urdf.links.length) throw new Error('No URDF links converted from USD Physics');

  const extractedCollisionCount = prims.filter((prim) => hasApi(prim, 'PhysicsCollisionAPI')).length;
  const urdfCollisionCount = urdf.links.reduce((sum, link) => sum + link.collisions.length, 0);
  if (extractedCollisionCount > 0) {
    if (!hasSchema('PhysicsCollisionAPI')) throw new Error('Missing extracted API schema: PhysicsCollisionAPI');
    if (!hasSchema('MjcCollisionAPI')) throw new Error('Missing extracted API schema: MjcCollisionAPI');
    if (urdfCollisionCount === 0) {
      throw new Error('No URDF collisions converted from authored USD Physics collisions');
    }
  }

  const extractedJointCount = prims.filter((prim) => /^Physics(?:Revolute|Prismatic|Fixed|Joint)/.test(prim.type)).length;
  if (extractedJointCount > 0 && urdf.joints.length === 0) {
    throw new Error('No URDF joints converted from authored USD Physics joints');
  }

  const hinge = urdf.joints.find((joint) => joint.name === 'hinge') || urdf.joints[0];

  if (expected) {
    if (!hasSchema('NewtonSceneAPI')) throw new Error('Missing extracted API schema: NewtonSceneAPI');
    if (!hinge) throw new Error('Expected sample hinge joint was not converted');
    if (hinge.type !== 'revolute') throw new Error(`Expected revolute joint, got ${hinge.type}`);
    if (Math.abs((hinge.limit?.lower || 0) - -1.25) > 1.0e-3) {
      throw new Error(`Revolute lower limit mismatch: ${hinge.limit?.lower}`);
    }
    if (Math.abs((hinge.limit?.upper || 0) - 1.25) > 1.0e-3) {
      throw new Error(`Revolute upper limit mismatch: ${hinge.limit?.upper}`);
    }
    if (Math.abs((hinge.dynamics?.damping || 0) - 0.1) > 1.0e-6) {
      throw new Error(`MuJoCo damping mismatch: ${hinge.dynamics?.damping}`);
    }
    if (Math.abs((hinge.dynamics?.friction || 0) - 0.02) > 1.0e-6) {
      throw new Error(`MuJoCo friction mismatch: ${hinge.dynamics?.friction}`);
    }
    if (urdf.links.length !== expected.links.length) {
      throw new Error(`Link count mismatch: ${urdf.links.length} != ${expected.links.length}`);
    }
    if (urdf.joints.length !== expected.joints.length) {
      throw new Error(`Joint count mismatch: ${urdf.joints.length} != ${expected.joints.length}`);
    }
    const base = urdf.links.find((link) => link.name === 'base_link');
    if (!base || Math.abs(base.inertial.mass - expected.links[0].inertial.mass) > 1.0e-6) {
      throw new Error(`Mass mismatch for base_link: ${base?.inertial.mass}`);
    }
    const collision = base.collisions[0];
    // Accept both the canonical GeomCube name ("cube") and the legacy "box"
    // alias (a Cube prim historically serialized as type "box").
    if (!collision || (collision.geometry.type !== 'box'
                       && collision.geometry.type !== 'cube')) {
      throw new Error(`Expected base box/cube collision, got ${collision?.geometry.type}`);
    }
    if (!hasSchema('NewtonCollisionAPI')) throw new Error('Missing extracted API schema: NewtonCollisionAPI');
    if (!collision.newton) throw new Error('Missing Newton collision metadata');
  }
}

async function extractFromInput(native, inputFile) {
  const inputPath = path.resolve(inputFile);
  const bytes = fs.readFileSync(inputPath);
  if (!native.loadFromBinary(bytes, path.basename(inputPath))) {
    throw new Error(native.error() || `Failed to load ${inputPath}`);
  }
  const jsonText = native.extractPhysicsSceneJSON();
  if (!jsonText) throw new Error(native.error() || 'extractPhysicsSceneJSON failed');
  return JSON.parse(jsonText);
}

function exportSampleBytes(native, format) {
  if (format === 'usda') {
    const usda = native.exportAsUSDA();
    if (!usda) throw new Error(native.error() || 'Sample USDA export failed');
    return { bytes: new TextEncoder().encode(usda), filename: 'roundtrip.usda' };
  }
  const usdc = native.exportAsUSDC();
  if (!usdc) throw new Error(native.error() || 'Sample USDC export failed');
  return { bytes: new Uint8Array(usdc), filename: 'roundtrip.usdc' };
}

async function extractFromSample(lightusd, format) {
  const expected = sampleRobotPayload();
  const writer = new lightusd.USDZConverterNative();
  try {
    if (!writer.createURDFPhysicsScene(JSON.stringify(expected))) {
      throw new Error(writer.error() || 'createURDFPhysicsScene failed');
    }
    const exported = exportSampleBytes(writer, format);
    const reader = new lightusd.USDZConverterNative();
    try {
      if (!reader.loadFromBinary(exported.bytes, exported.filename)) {
        throw new Error(reader.error() || `Failed to reload ${exported.filename}`);
      }
      const jsonText = reader.extractPhysicsSceneJSON();
      if (!jsonText) throw new Error(reader.error() || 'extractPhysicsSceneJSON failed');
      return { extracted: JSON.parse(jsonText), expected };
    } finally {
      reader.delete();
    }
  } finally {
    writer.delete();
  }
}

async function main() {
  const opts = parseArgs();
  const lightusd = await LightUSDFactory();
  const native = new lightusd.USDZConverterNative();
  let extracted;
  let expected = null;
  try {
    if (opts.sample) {
      const result = await extractFromSample(lightusd, opts.format);
      extracted = result.extracted;
      expected = result.expected;
    } else {
      extracted = await extractFromInput(native, opts.inputFile);
    }
  } finally {
    native.delete();
  }

  const urdf = usdPhysicsToUrdf(extracted);
  const urdfXML = urdfToXML(urdf);

  if (opts.assert) {
    assertRoundtrip(extracted, urdf, expected);
  }

  if (opts.dumpExtracted) console.log(JSON.stringify(extracted, null, 2));
  if (opts.dumpUrdfJson) console.log(JSON.stringify(urdf, null, 2));
  if (opts.dumpUrdf) console.log(urdfXML);

  const collisionCount = urdf.links.reduce((sum, link) => sum + link.collisions.length, 0);
  const visualCount = urdf.links.reduce((sum, link) => sum + link.visuals.length, 0);
  console.log(`Verified USD Physics -> URDF JS roundtrip: ${urdf.links.length} links, ${urdf.joints.length} joints, ${visualCount} visuals, ${collisionCount} collisions.`);
  if (opts.verbose) {
    console.log(`upAxis=${urdf.upAxis}, format=${opts.sample ? opts.format : path.extname(opts.inputFile).slice(1)}`);
    console.log(`limits(deg)=${urdf.joints.map((joint) => joint.limit ? `${joint.name}:${fmtNumber(joint.limit.lower * RAD_TO_DEG)}..${fmtNumber(joint.limit.upper * RAD_TO_DEG)}` : `${joint.name}:none`).join(', ')}`);
  }
}

main().catch((err) => {
  console.error(`usd-urdf-roundtrip: ${err.message}`);
  if (process.argv.includes('--verbose') || process.argv.includes('-v')) {
    console.error(err.stack);
  }
  process.exit(1);
});
