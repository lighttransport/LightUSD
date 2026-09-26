import glob
import os
import unittest

import _util
import miniusd
from miniusd import crate_format as C
from miniusd.usdc_reader import CrateReader

FEATURES = os.path.join(_util.DATA, "features.usda")


class TestUsdc(unittest.TestCase):
    def test_features_roundtrip(self):
        src = miniusd.open(FEATURES)
        data = src.to_usdc()
        self.assertEqual(data[:8], b"PXR-USDC")
        self.assertEqual(tuple(data[8:11]), C.WRITE_VERSION)
        back = miniusd.loads(data)
        self.assertTrue(miniusd.specs_equal(src, back))
        self.assertEqual(src.to_usda(), back.to_usda())

    def test_sections(self):
        r = CrateReader(miniusd.open(FEATURES).to_usdc())
        self.assertEqual(set(r.sections), {"TOKENS", "STRINGS", "FIELDS", "FIELDSETS", "PATHS", "SPECS"})
        self.assertEqual(r.paths[0], "/")
        self.assertIn("/World{shading=blue}InVariant", r.paths)
        self.assertIn("", r.paths)  # empty prim path of the payload arc

    def test_empty_layer(self):
        l = miniusd.loads(miniusd.Stage().to_usdc())
        self.assertEqual(l.root_prims, [])

    def test_models_roundtrip(self):
        files = sorted(glob.glob(os.path.join(_util.MODELS, "*.usda")))
        if not files:
            self.skipTest("repo models not found")
        for f in files:
            with self.subTest(f=os.path.basename(f)):
                l = miniusd.open(f)
                self.assertTrue(miniusd.specs_equal(l, miniusd.loads(l.to_usdc())))

    def test_read_pxr_written_files(self):
        files = sorted(glob.glob(os.path.join(_util.MODELS, "*.usdc")))
        if not files:
            self.skipTest("repo models not found")
        for f in files:
            with self.subTest(f=os.path.basename(f)):
                l = miniusd.open(f)
                # stable through our own writers
                self.assertTrue(miniusd.specs_equal(l, miniusd.loads(l.to_usdc())))
                self.assertTrue(miniusd.specs_equal(l, miniusd.loads(l.to_usda())))


if __name__ == "__main__":
    unittest.main()
