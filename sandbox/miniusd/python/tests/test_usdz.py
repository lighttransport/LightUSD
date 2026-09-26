import io
import unittest
import zipfile

import _util  # noqa: F401
import miniusd
from miniusd import geom


class TestUsdz(unittest.TestCase):
    def make(self):
        s = miniusd.Stage(up_axis="Y")
        m = geom.add_cube(s, "/World/Box")
        mat = geom.add_preview_material(s, "/World/Mat", diffuse_texture="tex/a.png")
        geom.bind_material(m, mat)
        return s

    def test_alignment_and_order(self):
        assets = {"tex/a.png": b"\x89PNG" + bytes(100), "tex/b.bin": bytes(7)}
        for fmt in ("usdc", "usda"):
            data = self.make().to_usdz(assets, root_format=fmt)
            zf = zipfile.ZipFile(io.BytesIO(data))
            infos = zf.infolist()
            self.assertEqual(infos[0].filename, "scene." + fmt)
            for zi in infos:
                self.assertEqual(zi.compress_type, zipfile.ZIP_STORED)
                start = zi.header_offset + 30 + len(zi.filename.encode()) + len(zi.extra)
                self.assertEqual(start % 64, 0, zi.filename)
                self.assertEqual(data[start:start + zi.file_size], zf.read(zi))

    def test_roundtrip(self):
        s = self.make()
        s.assets["tex/a.png"] = b"PNGDATA"
        back = miniusd.loads(s.to_usdz())
        self.assertTrue(miniusd.specs_equal(s, back))
        self.assertEqual(back.assets, {"tex/a.png": b"PNGDATA"})


if __name__ == "__main__":
    unittest.main()
