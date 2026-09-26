"""Differential tests against OpenUSD's `usdcat` (skipped when it is not found).

Set MINIUSD_USDCAT=/path/to/usdcat, or put usdcat on PATH. Paths are never
guessed from a developer's home directory.
"""

import glob
import os
import shutil
import subprocess
import tempfile
import unittest

import _util
import miniusd


def _find_usdcat():
    cand = [os.environ.get("MINIUSD_USDCAT"), shutil.which("usdcat")]
    for c in cand:
        if c and os.path.exists(c):
            return c
    return None


USDCAT = _find_usdcat()


def usdcat(path, out=None):
    cmd = [USDCAT, path] + (["-o", out] if out else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout


@unittest.skipIf(USDCAT is None, "usdcat not found")
class TestPxrInterop(unittest.TestCase):
    def files(self):
        fs = [os.path.join(_util.DATA, "features.usda")]
        fs += sorted(glob.glob(os.path.join(_util.MODELS, "*.usda")))
        return fs

    def test_three_ways(self):
        """pxr must see the same layer whether it reads the source, our .usdc,
        or our .usda; and we must read pxr's .usdc to the same layer."""
        with tempfile.TemporaryDirectory() as d:
            for f in self.files():
                rc, ref = usdcat(f)
                if rc != 0:
                    continue
                with self.subTest(f=os.path.basename(f)):
                    layer = miniusd.open(f)
                    for ext in ("usdc", "usda"):
                        out = os.path.join(d, "ours." + ext)
                        layer.save(out)
                        self.assertEqual(usdcat(out), (0, ref), ext)
                    pxr_c = os.path.join(d, "pxr.usdc")
                    usdcat(f, pxr_c)
                    out = os.path.join(d, "ours_from_pxr.usda")
                    miniusd.open(pxr_c).save(out)
                    self.assertEqual(usdcat(out), usdcat(pxr_c), "read pxr usdc")

    def test_usdz(self):
        from miniusd import geom
        s = miniusd.Stage(up_axis="Y")
        geom.add_cube(s, "/World/Box")
        with tempfile.TemporaryDirectory() as d:
            p = s.save(os.path.join(d, "a.usdz"))
            a = s.save(os.path.join(d, "a.usda"))
            self.assertEqual(usdcat(p), usdcat(a))


if __name__ == "__main__":
    unittest.main()
