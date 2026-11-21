import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';

import {
  buildMujocoPayload,
  expandMujocoIncludes,
  parseArgs,
  verifyUSDA
} from '../cli/urdf-to-usd.js';

function test(name, fn) {
  try {
    fn();
    console.log(`ok - ${name}`);
  } catch (err) {
    console.error(`not ok - ${name}`);
    console.error(err);
    process.exitCode = 1;
  }
}

async function testAsync(name, fn) {
  try {
    await fn();
    console.log(`ok - ${name}`);
  } catch (err) {
    console.error(`not ok - ${name}`);
    console.error(err);
    process.exitCode = 1;
  }
}

function withTempDir(fn) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lightusd-urdf-cli-'));
  let result;
  try {
    result = fn(dir);
  } catch (err) {
    fs.rmSync(dir, { recursive: true, force: true });
    throw err;
  }
  if (result && typeof result.finally === 'function') {
    return result.finally(() => {
      fs.rmSync(dir, { recursive: true, force: true });
    });
  }
  fs.rmSync(dir, { recursive: true, force: true });
  return result;
}

test('parseArgs recognizes unsafe path escape hatch', () => {
  const opts = parseArgs(['scene.xml', '--input-format', 'mjcf', '--allow-unsafe-paths']);
  assert.equal(opts.allowUnsafePaths, true);
});

function fakeNativeUSDAText(usdaText) {
  return {
    exportAsUSDA: () => usdaText,
    error: () => ''
  };
}

test('verifyUSDA does not require articulation roots for static-only scenes', () => {
  const usda = `
    #usda 1.0
    def Xform "World" (
      prepend apiSchemas = ["PhysicsScene", "MjcSceneAPI", "NewtonSceneAPI"]
    )
    {
      def Mesh "Ground" (
        prepend apiSchemas = [
          "PhysicsCollisionAPI",
          "MjcCollisionAPI",
          "NewtonCollisionAPI",
          "PhysicsRigidBodyAPI",
        ]
      )
      {
      }
    }
  `;
  const payload = {
    links: [{ name: 'world', static: true }]
  };
  assert.doesNotThrow(() => {
    verifyUSDA(fakeNativeUSDAText(usda), { links: 1, joints: 0, collisions: 1, visuals: 0, actuators: 0 }, payload);
  });
});

test('verifyUSDA requires articulation root APIs when dynamic root links exist', () => {
  const usdaWithoutRoots = `
    #usda 1.0
    def Xform "World" {
      def Xform "WorldBody" {}
    }
  `;
  const payload = {
    links: [{ name: 'base' }],
    joints: []
  };
  assert.throws(() => {
    verifyUSDA(fakeNativeUSDAText(usdaWithoutRoots), { links: 1, joints: 0, collisions: 0, visuals: 0, actuators: 0 }, payload);
  }, /Missing USDA tokens: .*PhysicsRigidBodyAPI.*NewtonArticulationRootAPI/);
});

test('verifyUSDA accepts typed physics joints for core backend scenes', () => {
  const usdaWithTypedJoint = `
    #usda 1.0
      def Xform "World" (
      prepend apiSchemas = ["PhysicsScene", "MjcSceneAPI", "NewtonSceneAPI"]
    )
    {
      def Xform "Base" (
        prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "PhysicsArticulationRootAPI", "NewtonArticulationRootAPI"]
      )
      {
      }
      def Xform "Tip" (
        prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]
      )
      {
      }
      def PhysicsRevoluteJoint "J"
      {
        physics:axis = "Z"
      }
    }
  `;
  const payload = {
    links: [{ name: 'base' }, { name: 'tip' }],
    joints: [{ name: 'J', parent: 'base', child: 'tip' }]
  };
  assert.doesNotThrow(() => {
    verifyUSDA(fakeNativeUSDAText(usdaWithTypedJoint), { links: 2, joints: 1, collisions: 0, visuals: 0, actuators: 0 }, payload);
  });
});

test('verifyUSDA accepts legacy MjcJointAPI when typed joints are not present', () => {
  const usdaWithMjcJoint = `
    #usda 1.0
      def Xform "World" (
      prepend apiSchemas = ["PhysicsScene", "MjcSceneAPI", "NewtonSceneAPI"]
    )
    {
      def Xform "Base" (
        prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "PhysicsArticulationRootAPI", "NewtonArticulationRootAPI"]
      )
      {
      }
      def Xform "Tip" (
        prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]
      )
      {
      }
      def Xform "J" (
        prepend apiSchemas = ["MjcJointAPI"]
      )
      {
      }
    }
  `;
  const payload = {
    links: [{ name: 'base' }, { name: 'tip' }],
    joints: [{ name: 'J', parent: 'base', child: 'tip' }]
  };
  assert.doesNotThrow(() => {
    verifyUSDA(fakeNativeUSDAText(usdaWithMjcJoint), { links: 2, joints: 1, collisions: 0, visuals: 0, actuators: 0 }, payload);
  });
});

test('MJCF includes inside input root are allowed', () => withTempDir((dir) => {
  const modelDir = path.join(dir, 'model');
  const partsDir = path.join(modelDir, 'parts');
  fs.mkdirSync(partsDir, { recursive: true });
  fs.writeFileSync(
    path.join(partsDir, 'child.xml'),
    '<mujoco><worldbody><body name="included"/></worldbody></mujoco>\n',
    'utf8'
  );

  const expanded = expandMujocoIncludes(
    '<mujoco><include file="parts/child.xml"/><worldbody/></mujoco>',
    modelDir,
    { assetDirs: [modelDir] }
  );
  assert.match(expanded, /body name="included"/);
}));

test('MJCF includes outside allowed roots are blocked by default', () => withTempDir((dir) => {
  const modelDir = path.join(dir, 'model');
  fs.mkdirSync(modelDir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'outside.xml'),
    '<mujoco><worldbody><body name="outside"/></worldbody></mujoco>\n',
    'utf8'
  );

  assert.throws(
    () => expandMujocoIncludes(
      '<mujoco><include file="../outside.xml"/><worldbody/></mujoco>',
      modelDir,
      { assetDirs: [modelDir] }
    ),
    /Blocked MJCF include outside allowed roots/
  );
}));

test('unsafe path flag preserves trusted legacy MJCF include behavior', () => withTempDir((dir) => {
  const modelDir = path.join(dir, 'model');
  fs.mkdirSync(modelDir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'outside.xml'),
    '<mujoco><worldbody><body name="outside"/></worldbody></mujoco>\n',
    'utf8'
  );

  const expanded = expandMujocoIncludes(
    '<mujoco><include file="../outside.xml"/><worldbody/></mujoco>',
    modelDir,
    { assetDirs: [modelDir], allowUnsafePaths: true }
  );
  assert.match(expanded, /body name="outside"/);
}));

await testAsync('MJCF ball joints and joint actuators are exported to payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="ActuatedBall">
  <worldbody>
    <body name="base">
      <body name="tip">
        <joint name="ball" type="ball"/>
        <geom name="tip_geom" type="sphere" size="0.05"/>
      </body>
    </body>
  </worldbody>
  <actuator>
    <position name="ball_drive" joint="ball" kp="5" kv="0.25" forcerange="-3 4"/>
    <motor name="tendon_motor" tendon="tendon0"/>
  </actuator>
</mujoco>`;

    const { payload, stats } = await buildMujocoPayload(
      xml,
      { assetDirs: [dir], upAxis: 'Z' },
      dir
    );

    assert.equal(payload.joints.length, 1);
    assert.equal(payload.joints[0].type, 'spherical');
    assert.equal(payload.actuators.length, 1);
    assert.equal(payload.actuators[0].name, 'ball_drive');
    assert.equal(payload.actuators[0].joint, 'ball');
    assert.equal(payload.actuators[0].kp, 5);
    assert.equal(payload.actuators[0].kd, 0.25);
    assert.equal(payload.actuators[0].maxEffort, 4);
    assert.equal(stats.actuators, 1);
  });
});

await testAsync('MJCF multiple joints per body -> intermediate-link chain', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="MultiDof">
  <worldbody>
    <body name="base">
      <body name="slider">
        <joint name="lift" type="slide" axis="0 0 1" range="0 0.5"/>
        <joint name="twist" type="hinge" axis="0 0 1" range="-90 90"/>
        <geom name="g" type="sphere" size="0.05"/>
      </body>
    </body>
  </worldbody>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    // Two joints chained via one massless intermediate link.
    assert.equal(payload.joints.length, 2);
    const lift = payload.joints.find((j) => j.name === 'lift');
    const twist = payload.joints.find((j) => j.name === 'twist');
    assert.ok(lift && twist, 'both joints present');
    assert.equal(lift.type, 'prismatic');
    assert.equal(twist.type, 'revolute');
    assert.equal(lift.parent, 'base');
    assert.equal(lift.child, 'slider__mjcdof_1');
    assert.equal(twist.parent, 'slider__mjcdof_1');
    assert.equal(twist.child, 'slider');
    assert.ok(payload.links.some((l) => l.name === 'slider__mjcdof_1'), 'intermediate link emitted');
  });
});

await testAsync('MJCF tendon / equality / contact-exclude / fullinertia -> payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Constraints">
  <worldbody>
    <body name="base">
      <inertial pos="0 0 0" mass="2" fullinertia="4 4 6 1 0 0"/>
      <geom name="bg" type="sphere" size="0.1"/>
      <body name="l"><joint name="jl" type="hinge" axis="0 1 0"/><geom type="sphere" size="0.05"/></body>
      <body name="r"><joint name="jr" type="hinge" axis="0 1 0"/><geom type="sphere" size="0.05"/></body>
    </body>
  </worldbody>
  <tendon>
    <fixed name="couple" stiffness="120" damping="3" armature="0.02"
           frictionloss="0.1" margin="0.01" group="2" limited="true"
           actuatorfrclimited="false" range="-1 1"
           actuatorfrcrange="-4 5" springlength="0.2 0.8"
           solreflimit="0.03 1.1" solimplimit="0.8 0.9 0.01 0.5 2">
      <joint joint="jl" coef="1"/>
      <joint joint="jr" coef="-1"/>
    </fixed>
  </tendon>
  <equality>
    <joint name="mirror" joint1="jl" joint2="jr" polycoef="0 -1 0 0 0"/>
    <weld name="lockit" body1="l" body2="r" torquescale="0.5"/>
  </equality>
  <contact>
    <exclude body1="l" body2="r"/>
  </contact>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    // Tendon
    assert.equal(payload.tendons.length, 1);
    assert.equal(payload.tendons[0].type, 'fixed');
    assert.equal(payload.tendons[0].joints.length, 2);
    assert.equal(payload.tendons[0].joints[0].coef, 1);
    assert.equal(payload.tendons[0].joints[1].coef, -1);
    assert.equal(payload.tendons[0].stiffness, 120);
    assert.equal(payload.tendons[0].damping, 3);
    assert.equal(payload.tendons[0].armature, 0.02);
    assert.equal(payload.tendons[0].frictionloss, 0.1);
    assert.equal(payload.tendons[0].margin, 0.01);
    assert.equal(payload.tendons[0].group, 2);
    assert.equal(payload.tendons[0].limited, 'true');
    assert.equal(payload.tendons[0].actuatorfrclimited, 'false');
    assert.deepEqual(payload.tendons[0].range, [-1, 1]);
    assert.deepEqual(payload.tendons[0].actuatorfrcrange, [-4, 5]);
    assert.deepEqual(payload.tendons[0].springlength, [0.2, 0.8]);
    assert.deepEqual(payload.tendons[0].solreflimit, [0.03, 1.1]);
    assert.deepEqual(payload.tendons[0].solimplimit, [0.8, 0.9, 0.01, 0.5, 2]);
    // Equality
    assert.equal(payload.equalities.length, 2);
    const mirror = payload.equalities.find((e) => e.name === 'mirror');
    const lockit = payload.equalities.find((e) => e.name === 'lockit');
    assert.equal(mirror.type, 'joint');
    assert.deepEqual(mirror.polycoef, [0, -1, 0, 0, 0]);
    assert.equal(lockit.type, 'weld');
    assert.equal(lockit.torquescale, 0.5);
    // Contact exclude -> filteredPairs
    assert.equal(payload.filteredPairs.length, 1);
    assert.equal(payload.filteredPairs[0].body1, 'l');
    // fullinertia carried as 6 components
    assert.deepEqual(payload.links[0].inertial.fullInertia, [4, 4, 6, 1, 0, 0]);
    // sourceFormat tag
    assert.equal(payload.sourceFormat, 'mjcf');
  });
});

await testAsync('MJCF mocap body + <custom> -> payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="MC">
  <custom>
    <numeric name="max_contact_points" data="8"/>
    <text name="description" data="demo"/>
  </custom>
  <worldbody>
    <body name="target" mocap="true"><geom type="sphere" size="0.05"/></body>
  </worldbody>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    const target = payload.links.find((l) => l.name === 'target');
    assert.equal(target.mocap, true);
    assert.ok(payload.custom);
    assert.equal(payload.custom.numeric[0].name, 'max_contact_points');
    assert.deepEqual(payload.custom.numeric[0].data, [8]);
    assert.equal(payload.custom.text[0].data, 'demo');
  });
});

await testAsync('MJCF <contact><pair> -> contactPairs payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Pair">
  <worldbody><body name="b"><geom name="foot" type="sphere" size="0.1"/></body></worldbody>
  <contact>
    <pair geom1="foot" geom2="floor" condim="6" friction="1 0.005 0.0001" solref="0.02 1" margin="0.001"/>
  </contact>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    assert.equal(payload.contactPairs.length, 1);
    const p = payload.contactPairs[0];
    assert.equal(p.geom1, 'foot');
    assert.equal(p.geom2, 'floor');
    assert.equal(p.condim, 6);
    assert.deepEqual(p.friction, [1, 0.005, 0.0001]);
    assert.equal(p.margin, 0.001);
  });
});

await testAsync('MJCF <adhesion>/<cylinder> actuators -> mjcActuators payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Act">
  <worldbody><body name="foot"><joint name="ankle" type="hinge"/><geom type="sphere" size="0.1"/></body></worldbody>
  <actuator>
    <adhesion name="grip" body="foot" gain="5"/>
    <cylinder name="piston" joint="ankle" ctrlrange="0 1"/>
  </actuator>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    assert.equal(payload.mjcActuators.length, 2);
    const grip = payload.mjcActuators.find((a) => a.name === 'grip');
    assert.equal(grip.actuatorType, 'adhesion');
    assert.equal(grip.targetBody, 'foot');
    assert.deepEqual(grip.gainPrm, [5]);
    const piston = payload.mjcActuators.find((a) => a.name === 'piston');
    assert.equal(piston.actuatorType, 'cylinder');
    assert.equal(piston.targetJoint, 'ankle');
  });
});

await testAsync('MJCF <sensor> -> sensors payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Sens">
  <worldbody>
    <body name="b"><joint name="j" type="hinge"/><site name="imu"/><geom type="sphere" size="0.1"/></body>
  </worldbody>
  <sensor>
    <gyro name="g" site="imu" cutoff="34.9" noise="0.01"/>
    <jointpos name="jp" joint="j"/>
    <framepos name="fp" body="b" refsite="imu" group="4" user="1 2 3"/>
    <tendonvel name="tv" tendon="t0"/>
  </sensor>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    assert.equal(payload.sensors.length, 4);
    const g = payload.sensors.find((s) => s.name === 'g');
    assert.equal(g.type, 'gyro');
    assert.equal(g.objtype, 'site');
    assert.equal(g.objname, 'imu');
    assert.equal(g.cutoff, 34.9);
    const jp = payload.sensors.find((s) => s.name === 'jp');
    assert.equal(jp.type, 'jointpos');
    assert.equal(jp.objtype, 'joint');
    const fp = payload.sensors.find((s) => s.name === 'fp');
    assert.equal(fp.type, 'framepos');
    assert.equal(fp.objtype, 'body');
    assert.equal(fp.objname, 'b');
    assert.equal(fp.reftype, 'site');
    assert.equal(fp.refname, 'imu');
    assert.equal(fp.group, 4);
    assert.deepEqual(fp.user, [1, 2, 3]);
    const tv = payload.sensors.find((s) => s.name === 'tv');
    assert.equal(tv.type, 'tendonvel');
    assert.equal(tv.objtype, 'tendon');
    assert.equal(tv.objname, 't0');
  });
});

await testAsync('MJCF <asset><material> -> materials payload + geom ref', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Mat">
  <asset>
    <material name="red" rgba="1 0 0 1" metallic="0.2" roughness="0.8"/>
    <material name="blue" rgba="0 0 1 1"/>
  </asset>
  <worldbody>
    <body name="b">
      <geom type="box" size="0.1 0.1 0.1" material="red"/>
    </body>
  </worldbody>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    assert.equal(payload.materials.length, 2);
    const red = payload.materials.find((m) => m.name === 'red');
    assert.deepEqual(red.rgba, [1, 0, 0, 1]);
    assert.equal(red.metallic, 0.2);
    assert.equal(red.roughness, 0.8);
    // The box visual references the material.
    const vis = payload.links[0].visuals.find((v) => v.material === 'red');
    assert.ok(vis, 'visual geom references material red');
  });
});

await testAsync('MJCF <light>/<camera> -> lights/cameras payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Lit">
  <worldbody>
    <light name="sun" directional="true" pos="0 0 3" dir="0 0 -1" diffuse="0.8 0.8 0.7"/>
    <camera name="cam" pos="0 -2 1" fovy="60"/>
    <body name="b">
      <light name="bulb" pos="0 0 1" diffuse="1 1 1"/>
      <geom type="sphere" size="0.1"/>
    </body>
  </worldbody>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    assert.equal(payload.lights.length, 2);
    const sun = payload.lights.find((l) => l.name === 'sun');
    const bulb = payload.lights.find((l) => l.name === 'bulb');
    assert.equal(sun.type, 'directional');
    assert.deepEqual(sun.color, [0.8, 0.8, 0.7]);
    assert.equal(sun.matrix.length, 16);
    assert.equal(bulb.type, 'spot');  // non-directional default
    assert.equal(payload.cameras.length, 1);
    assert.equal(payload.cameras[0].name, 'cam');
    assert.equal(payload.cameras[0].fovy, 60);
    assert.equal(payload.cameras[0].matrix.length, 16);
  });
});

await testAsync('MJCF <keyframe> -> keyframes payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Key">
  <worldbody><body name="b"><joint name="j" type="hinge"/><geom type="sphere" size="0.1"/></body></worldbody>
  <keyframe>
    <key name="init" qpos="0.5" ctrl="0.1"/>
    <key name="up" qpos="1.57"/>
  </keyframe>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    assert.equal(payload.keyframes.length, 2);
    assert.equal(payload.keyframes[0].name, 'init');
    assert.deepEqual(payload.keyframes[0].qpos, [0.5]);
    assert.deepEqual(payload.keyframes[0].ctrl, [0.1]);
    assert.equal(payload.keyframes[1].name, 'up');
    assert.deepEqual(payload.keyframes[1].qpos, [1.57]);
  });
});

await testAsync('MJCF <option>/<flag>/<compiler> -> mjcScene payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Opt">
  <compiler angle="radian" autolimits="true" boundmass="0.0001"/>
  <option timestep="0.001" integrator="implicitfast" solver="newton" cone="elliptic" iterations="50" ls_iterations="8">
    <flag eulerdamp="disable" contact="enable"/>
  </option>
  <worldbody><body name="b"><geom type="sphere" size="0.1"/></body></worldbody>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    assert.ok(payload.mjcScene, 'mjcScene present');
    assert.equal(payload.mjcScene.option.integrator, 'implicitfast');
    assert.equal(payload.mjcScene.option.solver, 'newton');
    assert.equal(payload.mjcScene.option.cone, 'elliptic');
    assert.equal(payload.mjcScene.option.iterations, 50);
    assert.equal(payload.mjcScene.option.ls_iterations, 8);
    assert.equal(payload.mjcScene.flag.eulerdamp, false);  // disable -> false
    assert.equal(payload.mjcScene.flag.contact, true);     // enable -> true
    assert.equal(payload.mjcScene.compiler.angle, 'radian');
    assert.equal(payload.mjcScene.compiler.autolimits, true);
    assert.equal(payload.mjcScene.compiler.boundmass, 0.0001);
  });
});

await testAsync('MJCF spatial (muscle) tendon + sites + muscle actuator -> payload', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Muscle">
  <default><tendon rgba="0.95 0.3 0.3 1" width="0.005"/></default>
  <worldbody>
    <body name="pelvis">
      <site name="m_p1" pos="0 0 0.1"/>
      <geom type="sphere" size="0.05"/>
      <site name="ref_site" pos="0.02 0 0"/>
      <body name="femur" pos="0 0 -0.3">
        <joint name="hip" type="hinge" axis="0 1 0"/>
        <site name="m_p2" pos="0.05 0 -0.2"/>
        <site name="slider_site" pos="0.02 0 -0.1"/>
        <geom type="capsule" fromto="0 0 0 0 0 -0.4" size="0.04"/>
      </body>
    </body>
  </worldbody>
  <tendon>
    <spatial name="glute_tendon" stiffness="10">
      <site site="m_p1"/>
      <pulley divisor="2.5"/>
      <site site="m_p2"/>
    </spatial>
  </tendon>
  <actuator>
    <general name="glute" class="muscle" tendon="glute_tendon"
             lengthrange="0.18 0.29" gainprm="0.75 1.05 916.8"
             biasprm="0.75 1.05 916.8" dynprm="0.01 0.02"
             ctrlrange="0 1" forcerange="-2 3" actrange="-0.5 0.5"
             gaintype="muscle" biastype="muscle" dyntype="filter"
             ctrllimited="true" forcelimited="false" actlimited="auto"
             group="4" actdim="2" cranklength="0.1" inheritrange="0.4"
             jointinparent="true" actearly="true"
             refsite="ref_site" slidersite="slider_site"/>
  </actuator>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    // Sites (with baked world matrices)
    assert.equal(payload.sites.length, 4);
    assert.ok(payload.sites.find((s) => s.name === 'm_p1'));
    assert.ok(payload.sites.find((s) => s.name === 'm_p2'));
    assert.equal(payload.sites[0].matrix.length, 16);
    // Spatial tendon routed through the two sites
    assert.equal(payload.tendons.length, 1);
    assert.equal(payload.tendons[0].type, 'spatial');
    assert.equal(payload.tendons[0].path.length, 2);
    assert.equal(payload.tendons[0].path[0].site, 'm_p1');
    assert.deepEqual(payload.tendons[0].routeDivisors, [2.5]);
    // Muscle actuator targeting the tendon
    assert.equal(payload.mjcActuators.length, 1);
    assert.equal(payload.mjcActuators[0].targetTendon, 'glute_tendon');
    assert.deepEqual(payload.mjcActuators[0].gainPrm, [0.75, 1.05, 916.8]);
    assert.deepEqual(payload.mjcActuators[0].biasPrm, [0.75, 1.05, 916.8]);
    assert.deepEqual(payload.mjcActuators[0].dynPrm, [0.01, 0.02]);
    assert.deepEqual(payload.mjcActuators[0].lengthRange, [0.18, 0.29]);
    assert.deepEqual(payload.mjcActuators[0].forceRange, [-2, 3]);
    assert.deepEqual(payload.mjcActuators[0].actRange, [-0.5, 0.5]);
    assert.equal(payload.mjcActuators[0].gainType, 'muscle');
    assert.equal(payload.mjcActuators[0].biasType, 'muscle');
    assert.equal(payload.mjcActuators[0].dynType, 'filter');
    assert.equal(payload.mjcActuators[0].ctrlLimited, 'true');
    assert.equal(payload.mjcActuators[0].forceLimited, 'false');
    assert.equal(payload.mjcActuators[0].actLimited, 'auto');
    assert.equal(payload.mjcActuators[0].group, 4);
    assert.equal(payload.mjcActuators[0].actDim, 2);
    assert.equal(payload.mjcActuators[0].crankLength, 0.1);
    assert.equal(payload.mjcActuators[0].inheritRange, 0.4);
    assert.equal(payload.mjcActuators[0].jointInParent, true);
    assert.equal(payload.mjcActuators[0].actEarly, true);
    assert.equal(payload.mjcActuators[0].refSite, 'ref_site');
    assert.equal(payload.mjcActuators[0].sliderSite, 'slider_site');
  });
});

await testAsync('MJCF <extension><plugin> + <actuator><plugin> -> plugins + mjcActuators', async () => {
  await withTempDir(async (dir) => {
    const xml = `<?xml version="1.0"?>
<mujoco model="Pid">
  <extension>
    <plugin plugin="mujoco.pid">
      <instance name="pid0">
        <config key="kp" value="2.8"/>
        <config key="ki" value="4.0"/>
      </instance>
    </plugin>
  </extension>
  <worldbody>
    <body name="link0"><joint name="j0" type="hinge"/><geom type="sphere" size="0.1"/></body>
  </worldbody>
  <actuator>
    <plugin name="act0" plugin="mujoco.pid" instance="pid0" joint="j0" ctrlrange="-1 1"/>
  </actuator>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    // Plugin instance declaration with its config.
    assert.equal(payload.plugins.length, 1);
    assert.equal(payload.plugins[0].instance, 'pid0');
    assert.equal(payload.plugins[0].plugin, 'mujoco.pid');
    assert.equal(payload.plugins[0].config.kp, '2.8');
    assert.equal(payload.plugins[0].config.ki, '4.0');
    // The plugin actuator routes to the MjcActuator path with plugin/instance...
    assert.equal(payload.mjcActuators.length, 1);
    assert.equal(payload.mjcActuators[0].name, 'act0');
    assert.equal(payload.mjcActuators[0].plugin, 'mujoco.pid');
    assert.equal(payload.mjcActuators[0].instance, 'pid0');
    assert.equal(payload.mjcActuators[0].targetJoint, 'j0');
    // ...and is NOT double-emitted on the Newton PD path (exclusivity, matches C++).
    assert.equal((payload.actuators || []).length, 0);
  });
});

await testAsync('MJCF <geom type="hfield"> in worldbody -> static "world" link mesh', async () => {
  await withTempDir(async (dir) => {
    // Inline heightfield (nrow/ncol/elevation) avoids a PNG fixture; the geom is
    // a direct <worldbody> child (world-fixed floor), not inside a <body>.
    const xml = `<?xml version="1.0"?>
<mujoco model="Terrain">
  <asset>
    <hfield name="terrain" nrow="3" ncol="3" size="2 2 1 0.1"
      elevation="0 0 0  0 1 0  0 0 0"/>
  </asset>
  <worldbody>
    <geom name="floor" type="hfield" hfield="terrain" material="ground"/>
  </worldbody>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    const world = payload.links.find((l) => l.name === 'world');
    assert.ok(world, 'a static "world" link is synthesized for worldbody geoms');
    assert.equal(world.static, true);
    // The hfield tessellated into a single visual mesh (referenced by meshRef).
    assert.equal(world.visuals.length, 1);
    assert.ok(world.visuals[0].meshRef, 'hfield geom became a mesh');
    assert.equal(world.visuals[0].material, 'ground');
    // It is ALSO a collider (default contype/conaffinity) — an exact triangle
    // mesh ('none'), sharing the visual's mesh buffer.
    assert.equal(world.collisions.length, 1);
    assert.equal(world.collisions[0].approximation, 'none');
    assert.equal(world.collisions[0].meshRef, world.visuals[0].meshRef);
  });
});

// Minimal valid 8-bit grayscale PNG encoder (filter 0 per scanline) for testing
// the file-based <hfield> decode path.
function makeGrayPNG(width, height, gray) {
  const u32 = (n) => { const b = Buffer.alloc(4); b.writeUInt32BE(n >>> 0); return b; };
  const chunk = (type, data) => {
    const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    return Buffer.concat([u32(data.length), body, u32(zlib.crc32(body))]);
  };
  const ihdr = Buffer.concat([u32(width), u32(height), Buffer.from([8, 0, 0, 0, 0])]);
  const raw = Buffer.alloc((width + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (width + 1)] = 0; // filter: none
    for (let x = 0; x < width; x++) raw[y * (width + 1) + 1 + x] = gray[y * width + x] & 0xff;
  }
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw)),
    chunk('IEND', Buffer.alloc(0))
  ]);
}

await testAsync('MJCF <hfield file=".png"> decodes + tessellates (file path)', async () => {
  await withTempDir(async (dir) => {
    // 4x4 ramp: the decoder must inflate + un-filter + collapse to luminance.
    const w = 4, h = 4, px = [];
    for (let i = 0; i < w * h; i++) px.push(Math.round((i / (w * h - 1)) * 255));
    fs.writeFileSync(path.join(dir, 'hf.png'), makeGrayPNG(w, h, px));
    const xml = `<?xml version="1.0"?>
<mujoco model="PngTerrain">
  <asset><hfield name="t" file="hf.png" size="1 1 0.5 0.1"/></asset>
  <worldbody><geom name="floor" type="hfield" hfield="t"/></worldbody>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    const world = payload.links.find((l) => l.name === 'world');
    assert.ok(world, 'PNG hfield produced a world link');
    assert.equal(world.visuals.length, 1);
    assert.ok(world.visuals[0].meshRef, 'PNG hfield decoded + tessellated to a mesh');
    assert.equal(world.collisions.length, 1, 'PNG hfield is also a collider');
  });
});

await testAsync('MJCF multiple <worldbody> blocks merge into one world', async () => {
  await withTempDir(async (dir) => {
    // Two <worldbody> blocks (as a post-<include> merge would produce): bodies
    // from BOTH must convert, not just the first.
    const xml = `<?xml version="1.0"?>
<mujoco model="Merged">
  <worldbody><body name="a"><geom type="sphere" size="0.1"/></body></worldbody>
  <worldbody><body name="b"><geom type="box" size="0.1 0.1 0.1"/></body></worldbody>
</mujoco>`;
    const { payload } = await buildMujocoPayload(xml, { assetDirs: [dir], upAxis: 'Z' }, dir);
    assert.ok(payload.links.find((l) => l.name === 'a'), 'body from worldbody #1');
    assert.ok(payload.links.find((l) => l.name === 'b'), 'body from worldbody #2');
  });
});
