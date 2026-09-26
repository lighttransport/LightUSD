"""UsdSkel / UsdMtlx / UsdPhysics helpers.

Optional oracle checks (skipped unless configured):
  MINIUSD_SKEL_ORACLE  path to a built tests/tools/skel_oracle (pxr UsdSkel evaluation)
  MINIUSD_PXR_PYTHON   python with `pxr` importable (schema type conformance)
  MINIUSD_MTLX_PYTHON  python with the `MaterialX` package (.mtlx validation)
"""

import glob
import math
import os
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

import _util
import miniusd
from miniusd import geom, mtlx, physics, skel

TOOLS = os.path.join(_util.HERE, "tools")
EXAMPLES = os.path.join(os.path.dirname(_util.HERE), "examples")


def as_rows(v):
    v = v.tolist() if hasattr(v, "tolist") else v
    return [tuple(float(x) for x in r) for r in v]


def two_joint_stage():
    s = miniusd.Stage(up_axis="Y")
    skel.add_skel_root(s, "/Root")
    joints = ["A", "A/B"]
    sk = skel.add_skeleton(s, "/Root/Skel", joints,
                           bind_transforms=[skel.IDENTITY, skel.translation_matrix((2, 0, 0))])
    pts = [(1, 0, 0), (3, 0, 0)]
    mesh = geom.add_mesh(s, "/Root/Mesh", pts, [2], [0, 1])
    skel.bind_skin(mesh, sk, [0, 1], [1.0, 1.0], element_size=1)
    skel.add_blendshape(mesh, "Up", [(0, 1, 0)], point_indices=[0])
    anim = skel.add_animation(
        s, "/Root/Skel/Anim", joints, translations=[(0, 0, 0), (2, 0, 0)],
        rotations={0: [(1, 0, 0, 0), (1, 0, 0, 0)],
                   10: [(1, 0, 0, 0), skel.quat_from_axis_angle((0, 0, 1), 90)]},
        scales=[(1, 1, 1), (1, 1, 1)], blend_shapes=["Up"], blend_shape_weights={0: [0.0], 10: [0.5]})
    skel.bind_animation(sk, anim)
    return s


class TestSkel(unittest.TestCase):
    def test_math(self):
        m = skel.trs_matrix((1, 2, 3), skel.quat_from_axis_angle((0, 1, 0), 30), (2, 2, 2))
        i = skel.mat_mul(m, skel.mat_inverse(m))
        for r in range(4):
            for c in range(4):
                self.assertAlmostEqual(i[r][c], 1.0 if r == c else 0.0)
        t, q, sc = skel.decompose_trs(m)
        self.assertEqual(tuple(round(x, 6) for x in t), (1, 2, 3))
        self.assertAlmostEqual(q[0], math.cos(math.radians(15)))
        self.assertEqual(tuple(round(x, 6) for x in sc), (2, 2, 2))

    def test_rest_from_bind(self):
        s = two_joint_stage()
        rest = skel.read_skeleton(s.prim_at("/Root/Skel"))["rest_transforms"]
        self.assertEqual(rest[1][3][:3], (2.0, 0.0, 0.0))
        self.assertEqual(skel.joint_parents(["A", "A/B", "C"]), [-1, 0, -1])

    def test_skinning_and_blendshape(self):
        s = two_joint_stage()
        p0 = as_rows(skel.evaluate_points(s, "/Root/Mesh", 0))
        self.assertEqual([tuple(round(x, 6) for x in p) for p in p0], [(1, 0, 0), (3, 0, 0)])
        p10 = as_rows(skel.evaluate_points(s, "/Root/Mesh", 10))
        self.assertEqual([tuple(round(x, 6) for x in p) for p in p10], [(1, 0.5, 0), (2, 1, 0)])
        p5 = as_rows(skel.evaluate_points(s, "/Root/Mesh", 5))  # slerp: 45 degrees
        c = math.sqrt(0.5)
        self.assertAlmostEqual(p5[1][0], 2 + c, places=5)
        self.assertAlmostEqual(p5[1][1], c, places=5)
        self.assertAlmostEqual(p5[0][1], 0.25, places=6)

    def test_roundtrip_formats(self):
        s = two_joint_stage()
        for data in (s.to_usdc(), s.to_usda()):
            s2 = miniusd.loads(data)
            self.assertTrue(miniusd.specs_equal(s, s2))
            p = as_rows(skel.evaluate_points(s2, "/Root/Mesh", 10))[1]
            self.assertEqual(tuple(round(x, 6) for x in p), (2.0, 1.0, 0.0))

    def test_repo_models(self):
        files = sorted(glob.glob(os.path.join(_util.MODELS, "skintest*.usda")))
        if not files:
            self.skipTest("repo models not found")
        for f in files:
            s = miniusd.open(f)
            for m in [p for p in s.traverse() if p.type_name == "Mesh"]:
                self.assertEqual(len(as_rows(skel.evaluate_points(s, m, 1))), len(as_rows(m.get("points"))))


class TestMtlx(unittest.TestCase):
    def test_parse_nodedef(self):
        self.assertEqual(mtlx.parse_nodedef("ND_open_pbr_surface_surfaceshader"),
                         ("open_pbr_surface", "surfaceshader"))
        self.assertEqual(mtlx.parse_nodedef("ND_convert_color4_color3"), ("convert", "color3"))
        self.assertEqual(mtlx.parse_nodedef("ND_multiply_color3FA"), ("multiply", "color3"))
        self.assertEqual(mtlx.infer_nodedef("dotproduct", "float", {"in1": "vector3"}),
                         "ND_dotproduct_vector3")
        self.assertEqual(mtlx.infer_nodedef("multiply", "vector3", {"in1": "vector3", "in2": "float"}),
                         "ND_multiply_vector3FA")

    def test_openpbr_material(self):
        s = miniusd.Stage()
        m = mtlx.add_openpbr_material(s, "/Looks/M", base_color=(1, 0, 0), roughness=0.4,
                                      base_color_texture="c.png", normal_texture="n.png")
        self.assertIn("MaterialXConfigAPI", m.metadata["apiSchemas"].items())
        self.assertEqual(m.get("config:mtlx:version"), mtlx.DEFAULT_VERSION)
        info = mtlx.read_material(s, m)
        self.assertEqual(info["surface"]["id"], "ND_open_pbr_surface_surfaceshader")
        self.assertEqual(info["preview"]["id"], "UsdPreviewSurface")
        self.assertEqual(sorted(info["textures"]), ["c.png", "n.png"])
        bc = info["surface"]["inputs"]["base_color"]
        self.assertEqual(bc["node"]["id"], "ND_image_color3")

    def test_xml_roundtrip(self):
        s = miniusd.Stage()
        mtlx.add_standard_surface_material(s, "/Looks/S", base_color=(0.2, 0.3, 0.4), metalness=1.0,
                                           roughness_texture="r.png")
        x1 = mtlx.material_to_mtlx(s, "/Looks/S")
        root = ET.fromstring(x1)
        self.assertEqual(root.find("standard_surface").get("type"), "surfaceshader")
        s2 = miniusd.Stage()
        mats = mtlx.mtlx_to_material(s2, "/Looks", x1)
        self.assertEqual([m.name for m in mats], ["S"])
        self.assertEqual(mtlx.material_to_mtlx(s2, "/Looks/S"), x1)

    def test_blender_file(self):
        f = os.path.join(_util.MODELS, "cube-mtlx-texture.usda")
        if not os.path.exists(f):
            self.skipTest("repo models not found")
        s = miniusd.open(f)
        m = [p for p in s.traverse() if p.type_name == "Material"][0]
        info = mtlx.read_material(s, m)
        self.assertEqual(info["mtlx_version"], "1.39")
        self.assertEqual(info["textures"], ["./textures/texture-cat.jpg"])
        ET.fromstring(mtlx.material_to_mtlx(s, m))

    def test_import_mtlx_library(self):
        files = sorted(glob.glob(os.path.join(_util.REPO, "data", "materialx", "*", "*.mtlx")))
        if not files:
            self.skipTest("repo materialx data not found")
        for f in files:
            s = miniusd.Stage()
            mats = mtlx.mtlx_file_to_material(s, "/Looks", f)
            self.assertTrue(mats, f)
            self.assertTrue(miniusd.specs_equal(s, miniusd.loads(s.to_usdc())))


class TestPhysics(unittest.TestCase):
    def build(self):
        s = miniusd.Stage(up_axis="Z")
        physics.add_scene(s, "/World/Scene", gravity_direction=(0, 0, -1), gravity_magnitude=9.8)
        a = geom.add_cube(s, "/World/A")
        b = geom.add_sphere(s, "/World/B")
        physics.add_rigid_body(a, mass=1.0, velocity=(1, 0, 0), starts_asleep=False)
        physics.add_collider(a, approximation="convexHull")
        physics.add_rigid_body(b, density=100.0)
        physics.add_collider(b)
        mat = physics.add_material(s, "/World/PhysMat", static_friction=0.9, restitution=0.5)
        physics.bind_material(a, mat)
        j = physics.add_joint(s, "/World/J", "revolute", a, b, axis="Y", lower_limit=-10, upper_limit=10,
                              exclude_from_articulation=True)
        physics.add_drive(j, "angular", "force", stiffness=10, damping=1)
        physics.add_limit(j, "rotX", -5, 5)
        physics.filter_pairs(a, b)
        physics.add_collision_group(s, "/World/G", members=[a])
        return s

    def test_describe(self):
        d = physics.describe(self.build())
        self.assertEqual(len(d["scenes"]), 1)
        self.assertEqual([b["path"] for b in d["rigid_bodies"]], ["/World/A", "/World/B"])
        self.assertEqual(d["colliders"][0]["approximation"], "convexHull")
        self.assertAlmostEqual(d["materials"][0]["staticFriction"], 0.9, places=6)
        j = d["joints"][0]
        self.assertEqual((j["type"], j["axis"], j["body0"]), ("PhysicsRevoluteJoint", "Y", ["/World/A"]))
        self.assertEqual(j["drives"]["angular"]["stiffness"], 10.0)
        self.assertEqual(j["limits"]["rotX"]["low"], -5.0)
        self.assertEqual(d["collision_groups"][0]["members"], ["/World/A"])

    def test_roundtrip(self):
        s = self.build()
        self.assertTrue(miniusd.specs_equal(s, miniusd.loads(s.to_usdc())))
        self.assertTrue(miniusd.specs_equal(s, miniusd.loads(s.to_usda())))

    def test_blender_file(self):
        f = os.path.join(_util.MODELS, "blender-physics.usda")
        if not os.path.exists(f):
            self.skipTest("repo models not found")
        d = physics.describe(miniusd.open(f))
        self.assertEqual(len(d["rigid_bodies"]), 10)
        self.assertEqual(len(d["joints"]), 2)


class TestFeaturesExample(unittest.TestCase):
    """Builds examples/build_features.py and runs the optional external oracles."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, EXAMPLES)
        import build_features
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = cls.tmp.name
        cls.stage = build_features.build()
        cls.usda = cls.stage.save(os.path.join(cls.dir, "features.usda"))
        cls.usdc = cls.stage.save(os.path.join(cls.dir, "features.usdc"))
        cls.mtlx = os.path.join(cls.dir, "Skin.mtlx")
        with open(cls.mtlx, "w") as f:
            f.write(mtlx.material_to_mtlx(cls.stage, "/World/Looks/Skin"))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_self_consistent(self):
        self.assertTrue(miniusd.specs_equal(self.stage, miniusd.open(self.usdc)))

    @unittest.skipUnless(os.environ.get("MINIUSD_SKEL_ORACLE"), "MINIUSD_SKEL_ORACLE not set")
    def test_skel_vs_pxr(self):
        for t in (1.0, 6.5, 24.0):
            out = subprocess.run([os.environ["MINIUSD_SKEL_ORACLE"], self.usdc, str(t)],
                                 capture_output=True, text=True, check=True).stdout.split("\n")
            path, n = out[0].split()
            ref = [tuple(map(float, ln.split())) for ln in out[1:1 + int(n)]]
            mine = as_rows(skel.evaluate_points(self.stage, path, t))
            err = max(abs(a - b) for p, q in zip(ref, mine) for a, b in zip(p, q))
            self.assertLess(err, 1e-5, "t=%s" % t)

    @unittest.skipUnless(os.environ.get("MINIUSD_PXR_PYTHON"), "MINIUSD_PXR_PYTHON not set")
    def test_schema_conformance(self):
        out = subprocess.run([os.environ["MINIUSD_PXR_PYTHON"], os.path.join(TOOLS, "pxr_schema_check.py"),
                              self.usda], capture_output=True, text=True, check=True).stdout
        # OpenUSD < 23.11 does not know MaterialXConfigAPI yet
        issues = [ln for ln in out.splitlines() if ln.startswith("ISSUE") and "MaterialXConfigAPI" not in ln]
        self.assertEqual(issues, [])
        self.assertGreater(int(out.split("CHECKED")[-1]), 50)

    @unittest.skipUnless(os.environ.get("MINIUSD_MTLX_PYTHON"), "MINIUSD_MTLX_PYTHON not set")
    def test_mtlx_valid(self):
        out = subprocess.run([os.environ["MINIUSD_MTLX_PYTHON"], os.path.join(TOOLS, "mtlx_validate.py"),
                              self.mtlx], capture_output=True, text=True, check=True).stdout
        self.assertIn(" valid ", out)


if __name__ == "__main__":
    unittest.main()
