import math
import os
import unittest

import _util
import miniusd
from miniusd import BLOCK, AssetPath, ListOp, Reference, TypedValue, UnregisteredValue
from miniusd.usda_reader import ParseError
from miniusd.values import values_equal

FEATURES = os.path.join(_util.DATA, "features.usda")


def as_list(v):
    return v.tolist() if hasattr(v, "tolist") else [list(x) if isinstance(x, tuple) else x for x in v]


class TestUsdaRead(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.layer = miniusd.open(FEATURES)

    def test_layer_metadata(self):
        m = self.layer.metadata
        self.assertEqual(m["defaultPrim"], "World")
        self.assertEqual(m["comment"], "layer comment")
        self.assertEqual(m["documentation"], "multi-line\ndocumentation")
        self.assertEqual(m["metersPerUnit"], 0.01)
        self.assertEqual(m["subLayers"], ["./sub_a.usda", "./sub_b.usda"])
        self.assertEqual(m["subLayerOffsets"], [(10.0, 2.0), (0.0, 1.0)])
        nested = m["customLayerData"]["nested"]
        self.assertEqual(nested["count"], 3)
        self.assertEqual(nested["dir"], TypedValue("float3", (0, 0.5, 1)))
        self.assertEqual(nested["t"], TypedValue("token", "tok"))
        self.assertEqual(m["unknownLayerMeta"], UnregisteredValue("42"))
        self.assertEqual(m["unknownLayerMeta"].value, 42)

    def test_prim_metadata(self):
        w = self.layer.prim_at("/World")
        self.assertEqual((w.specifier, w.type_name, w.kind), ("def", "Xform", "assembly"))
        refs = w.metadata["references"]
        self.assertEqual(refs.prepended, [Reference("./asset.usda", "/Asset", 5.0), Reference("", "/Internal")])
        self.assertEqual(w.metadata["inherits"], ListOp(explicit=["/_class_Base"]))
        self.assertEqual(w.metadata["specializes"], ListOp(deleted=["/Spec"]))
        self.assertEqual(w.metadata["apiSchemas"].prepended, ["GeomModelAPI", "CollectionAPI:foo"])
        self.assertEqual(w.metadata["variantSelection"], {"shading": "red"})
        self.assertEqual(w.metadata["customData"]["tex"], AssetPath("tex.png"))
        self.assertEqual(w.metadata["primOrder"], ["Mesh", "Mat"])
        self.assertEqual(list(w.variant_sets["shading"]), ["blue", "red"])
        blue = w.variant_sets["shading"]["blue"]
        self.assertEqual(blue.path, "/World{shading=blue}")
        self.assertEqual(blue.metadata["kind"], "subcomponent")
        self.assertEqual(blue.children["InVariant"].path, "/World{shading=blue}InVariant")
        self.assertEqual(self.layer.prim_at("/World/Overridden").specifier, "over")
        self.assertEqual(self.layer.prim_at("/_class_Base").specifier, "class")

    def test_values(self):
        m = self.layer.prim_at("/World/Mesh")
        self.assertEqual(as_list(m.get("points"))[2], [1, 1, 0])
        self.assertEqual(m.attribute("normals").metadata["interpolation"], "vertex")
        sp = as_list(m.get("specials"))
        self.assertTrue(math.isinf(sp[0]) and sp[1] < 0 and math.copysign(1, sp[3]) < 0)
        self.assertEqual(m.get("h2"), (0.5, 1.5))
        self.assertEqual(m.get("big"), 9007199254740993)
        self.assertEqual(m.get("ubig"), 2**64 - 1)
        self.assertEqual(m.get("s"), 'quote " and \\ backslash\nnewline')
        self.assertEqual(m.get("textures"), ["a.png", "b.png"])
        self.assertEqual(m.get("weird"), "odd@path")
        self.assertEqual(m.get("m2"), ((1.0, 2.0), (3.0, 4.0)))
        self.assertEqual(as_list(m.get("qd"))[1], [0.5, 0.5, 0.5, 0.5])
        self.assertIs(m.attribute("blocked").default, BLOCK)
        self.assertTrue(m.attribute("subdivisionScheme").uniform)
        self.assertTrue(m.attribute("userValue").custom)
        self.assertEqual(m.relationship("material:binding").get_targets(), ["/World/Mat"])
        self.assertEqual(m.relationship("proxyPrim").targets, ListOp(prepended=["/World/Proxy"]))
        self.assertTrue(m.relationship("legacy").varying)
        w = self.layer.prim_at("/World")
        ts = w.attribute("xformOp:rotateZ").time_samples
        self.assertEqual(list(ts), [1.0, 5.0, 10.0])
        self.assertIs(ts[5.0], BLOCK)
        self.assertEqual(w.attribute("xformOp:rotateZ").get(time=11), 360.0)
        self.assertEqual(w.get("xformOp:translate"), (1.0, -0.0, 3.25))
        mat = self.layer.prim_at("/World/Mat")
        self.assertEqual(mat.attribute("outputs:surface").connections.items(),
                         ["/World/Mat/Surface.outputs:surface"])
        self.assertEqual(self.layer.prim_at("/World/Mat/Surface").attribute("inputs:roughness").connections,
                         ListOp(explicit=[]))

    def test_text_roundtrip_is_stable(self):
        text = self.layer.to_usda()
        again = miniusd.loads(text)
        self.assertTrue(miniusd.specs_equal(self.layer, again))
        self.assertEqual(again.to_usda(), text)

    def test_relative_paths(self):
        l = miniusd.loads('#usda 1.0\ndef "A" {\n rel r = [<B>, <../C>, <.attr>]\n def "B" {}\n}\n')
        self.assertEqual(l.prim_at("/A").relationship("r").get_targets(), ["/A/B", "/C", "/A.attr"])

    def test_comments_and_errors(self):
        l = miniusd.loads("#usda 1.0\n# comment\ndef \"A\" { // c\n /* block */ int x = 1 }\n")
        self.assertEqual(l.prim_at("/A").get("x"), 1)
        with self.assertRaises(ParseError):
            miniusd.loads('#usda 1.0\ndef "A" { int x = }\n')
        with self.assertRaises(ValueError):
            miniusd.loads(b"not usd")


class TestRepoModels(unittest.TestCase):
    def test_models_roundtrip(self):
        import glob
        files = sorted(glob.glob(os.path.join(_util.MODELS, "*.usda")))
        if not files:
            self.skipTest("repo models not found")
        for f in files:
            with self.subTest(f=os.path.basename(f)):
                l = miniusd.open(f)
                l2 = miniusd.loads(l.to_usda())
                self.assertTrue(miniusd.specs_equal(l, l2))


class TestValues(unittest.TestCase):
    def test_infer(self):
        from miniusd.values import infer_type
        self.assertEqual(infer_type(1), "int")
        self.assertEqual(infer_type(1.0), "double")
        self.assertEqual(infer_type((1.0, 2.0, 3.0)), "double3")
        self.assertEqual(infer_type(["a"]), "string[]")
        self.assertEqual(infer_type({"a": 1}), "dictionary")
        self.assertEqual(infer_type(((1.0, 0.0), (0.0, 1.0))), "matrix2d")

    def test_coerce_float32(self):
        from miniusd.values import coerce
        self.assertEqual(coerce("float", 0.1), 0.10000000149011612)
        self.assertTrue(values_equal(coerce("float3[]", [0, 1, 2, 3, 4, 5]),
                                     coerce("float3[]", [(0, 1, 2), (3, 4, 5)])))


if __name__ == "__main__":
    unittest.main()
