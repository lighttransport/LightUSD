"""Opt-in tests using the local OpenUSD and native renderer installations."""
import json
import os
from pathlib import Path
import tempfile
import unittest

import config
import runner


@unittest.skipUnless(os.environ.get('OUSDDIFF_RUN_INTEGRATION') == '1', 'set OUSDDIFF_RUN_INTEGRATION=1 for real CPU render integration')
class IntegrationTests(unittest.TestCase):
    def test_instance_proxy_renderability_and_generated_camera(self):
        with tempfile.TemporaryDirectory(prefix='ousddiff-instance-') as directory:
            root = Path(directory)
            scene = root / 'instance.usda'
            scene.write_text('''#usda 1.0
(upAxis = "Y")
class Xform "Template" {
 def Mesh "Triangle" {
  point3f[] points = [(-1,-1,0),(1,-1,0),(0,1,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
 }
}
def Xform "Instance" (
 prepend inherits = </Template>
 instanceable = true
) {}
''')
            cfg = config.load(config.HERE / 'presets.json')
            cfg['render'].update(width=96, height=64)
            cfg['resources'].update(timeout_s=30, max_rss_mib=2048)
            prepared, _ = runner.prepare_case({'path': str(scene)}, cfg, root)
            self.assertEqual(prepared['status'], 'ok', prepared)
            self.assertEqual(prepared['renderable_count'], 1)
            self.assertEqual(prepared['classification'], 'renderable')
            self.assertEqual((prepared['width'], prepared['height']), (96, 64))
            self.assertIn('camera_transform', prepared)

    def test_shared_camera_variants_frames_resume_and_history(self):
        with tempfile.TemporaryDirectory(prefix='ousddiff-integration-') as directory:
            root = Path(directory)
            scene = root / 'input.usda'
            scene.write_text('''#usda 1.0
(
 upAxis = "Y"
 startTimeCode = 2
 endTimeCode = 3
)
def Xform "World" (
 variants = { string lod = "proxy" }
 prepend variantSets = "lod"
) {
 variantSet "lod" = {
  "proxy" {
   def Mesh "Mesh" {
    point3f[] points = [(-1,-1,0),(1,-1,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
   }
  }
  "full" {
   def Mesh "Mesh" {
    point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    uniform token subdivisionScheme = "none"
   }
  }
 }
}
def Camera "AuthoredCamera" {
 float horizontalAperture = 20
 float verticalAperture = 15
 float focalLength = 50
 double3 xformOp:translate = (0,0,10)
 uniform token[] xformOpOrder = ["xformOp:translate"]
}
''')
            cfg = json.loads((config.HERE / 'presets.json').read_text())
            cfg['datasets'] = []
            cfg['cases'] = [{'id': 'fixture', 'path': str(scene), 'variant_rules': [{'set': 'lod', 'selection': 'full'}]}]
            cfg['render'].update(width=64, height=48, camera='/AuthoredCamera', frame=2)
            cfg['renderers'] = [r for r in cfg['renderers'] if r['name'] in ('embree', 'cpu')]
            cfg['pairs'] = [cfg['pairs'][0]]
            cfg['resources'].update(timeout_s=30, max_rss_mib=2048)
            cfg['historical_thresholds'] = {'normalized_rmse': {'max': 0.03}, 'ssim': {'min': 0.95}}
            settings = root / 'config.json'
            settings.write_text(json.dumps(cfg))
            output = root / 'first'
            argv = ['run', '--config', str(settings), '--sample-percent', '1', '--no-full-scenes', '--frames', '2', '3', '--out', str(output)]
            self.assertEqual(runner.main(argv), 0)
            result = json.loads((output / 'results.json').read_text())
            self.assertEqual(len(result['cases']), 2)
            for case in result['cases']:
                self.assertEqual(case['prepared']['camera'], '/AuthoredCamera')
                self.assertEqual(case['prepared']['height'], 48)
                self.assertEqual(case['prepared']['variant_overrides'][0]['selection'], 'full')
                for render in case['renders'].values():
                    self.assertEqual(render['status'], 'ok')
            self.assertEqual(runner.main(argv + ['--resume']), 0)
            resumed = json.loads((output / 'results.json').read_text())
            self.assertTrue(all(case['resumed'] for case in resumed['cases']))
            second = root / 'second'
            history_argv = argv[:-2] + ['--out', str(second), '--baseline', str(output / 'results.json')]
            self.assertEqual(runner.main(history_argv), 0)
            historical = json.loads((second / 'results.json').read_text())
            self.assertTrue(all(case['historical']['status'] == 'pass' for case in historical['cases']))


if __name__ == '__main__':
    unittest.main()
