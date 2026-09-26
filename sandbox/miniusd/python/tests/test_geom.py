import os
import tempfile
import unittest

import _util  # noqa: F401
import miniusd
from miniusd import geom


class TestGeom(unittest.TestCase):
    def test_build_and_save_all_formats(self):
        s = miniusd.Stage(up_axis="Y", meters_per_unit=1)
        geom.add_xform(s, "/World", translate=(0, 1, 0))
        pts, counts, idx, nrm = geom.uv_sphere_mesh(1.0, 8, 4)
        ball = geom.add_mesh(s, "/World/Ball", pts, counts, idx, normals=nrm, display_color=(1, 0, 0))
        mat = geom.add_preview_material(s, "/World/Looks/M", diffuse_color=(0.5, 0.5, 0.5))
        geom.bind_material(ball, mat)
        geom.add_camera(s, "/World/Cam", translate=(0, 0, 10))
        geom.add_distant_light(s, "/World/Sun")
        ball.set("primvars:myScalar", 1.5)
        self.assertEqual(ball.attribute("primvars:myScalar").type_name, "float")
        self.assertEqual(ball.attribute("points").type_name, "point3f[]")
        self.assertEqual(s.default_prim, "World")
        ext = ball.get("extent")
        self.assertAlmostEqual(float(ext[1][0]), 1.0, places=6)
        with tempfile.TemporaryDirectory() as d:
            for ext_ in ("usda", "usdc", "usd", "usdz"):
                p = s.save(os.path.join(d, "scene." + ext_))
                self.assertTrue(miniusd.specs_equal(s, miniusd.open(p)), ext_)

    def test_prim_api(self):
        s = miniusd.Stage()
        p = s.define("/A/B/C", "Xform")
        self.assertEqual(p.path, "/A/B/C")
        self.assertEqual(s.prim_at("/A/B").type_name, "")
        p.add_reference("x.usda", "/X")
        v = p.variant("look", "red")
        v.set("color", (1, 0, 0), "color3f")
        p.set_variant_selection("look", "red")
        self.assertEqual(p.metadata["variantSetNames"].items(), ["look"])
        self.assertEqual(s.property_at("/A/B/C.missing"), None)
        self.assertIsNotNone(s.remove_prim("/A/B/C"))
        self.assertIsNone(s.prim_at("/A/B/C"))


if __name__ == "__main__":
    unittest.main()
