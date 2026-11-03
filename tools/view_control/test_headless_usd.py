"""USD-only preparation checks; run using the installed fork's Python environment."""
import hashlib
from pathlib import Path
import tempfile
import unittest
from headless import normalize
from headless_stage import prepare
try:
    from pxr import Gf,Sdf,Usd,UsdGeom,UsdUtils
except ImportError:
    Usd=None


@unittest.skipUnless(Usd,'requires installed OpenUSD fork')
class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory();self.root=Path(self.temporary.name)
        self.source=self.root/'source.usda';self.source.write_text('#usda 1.0\ndef Cube "World" {\n double size = 2\n}\n')
        self.original=self.source.read_bytes()

    def tearDown(self):
        self.assertEqual(self.source.read_bytes(),self.original)
        self.temporary.cleanup()

    def prepare(self,changes=None):
        state=normalize({'scene':str(self.source)})
        if changes: state.update(changes)
        directory=self.root/'prepared';directory.mkdir()
        return prepare({'state':normalize(state),'directory':str(directory)})

    def test_frozen_camera_authored_at_default_and_metadata_preview(self):
        result=self.prepare()
        stage=Usd.Stage.Open(result['wrapper']);camera=UsdGeom.Camera(stage.GetPrimAtPath(result['camera_path']))
        self.assertAlmostEqual(camera.GetCamera().GetFieldOfView(Gf.Camera.FOVVertical),60,places=4)
        self.assertEqual(camera.GetFocalLengthAttr().GetNumTimeSamples(),0)
        self.assertTrue(any(item['path']==str(self.source) for item in result['dependencies']))
        self.assertEqual(UsdGeom.GetStageUpAxis(stage),UsdGeom.GetStageUpAxis(Usd.Stage.Open(str(self.source))))

    def test_shared_edit_and_selected_bounds(self):
        result=self.prepare({'edits':[{'op':'set_attribute','path':'/World','property':'size','value':.3}],
                             'selection':['/World']})
        stage=Usd.Stage.Open(result['wrapper'])
        self.assertAlmostEqual(stage.GetPrimAtPath('/World').GetAttribute('size').Get(),.3)
        self.assertGreaterEqual(result['diagnostic_segments'],8)

    def test_package_dependencies_track_archive(self):
        package=self.root/'scene.usdz'
        self.assertTrue(UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath(str(self.source)),str(package)))
        original=hashlib.sha256(package.read_bytes()).hexdigest()
        result=self.prepare({'scene':str(package)})
        self.assertTrue(any(item['path']==str(package) for item in result['dependencies']))
        self.assertEqual(hashlib.sha256(package.read_bytes()).hexdigest(),original)


if __name__=='__main__': unittest.main()
