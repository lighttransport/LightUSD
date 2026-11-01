"""Opt-in AMD hardware matrix. Run outside the sandbox with render-node access."""
import json
import os
from pathlib import Path
import tempfile
import unittest

import config
import runner


@unittest.skipUnless(os.environ.get('OUSDDIFF_RUN_AMD_INTEGRATION') == '1', 'set OUSDDIFF_RUN_AMD_INTEGRATION=1 with host AMD GPU access')
class AmdIntegrationTests(unittest.TestCase):
    def test_offscreen_amd_compute_and_ray_query(self):
        with tempfile.TemporaryDirectory(prefix='ousddiff-amd-integration-') as directory:
            root = Path(directory)
            cfg = json.loads((config.HERE / 'presets.json').read_text())
            cfg['datasets'] = []
            cfg['cases'] = [{'id': 'hardware/mesh', 'path': str(config.REPO / 'tests/usda/suzanne.usda'), 'full_scene': False}]
            cfg['render'].update(width=96, height=64)
            cfg['resources'].update(timeout_s=45, max_rss_mib=4096)
            settings = root / 'config.json'
            settings.write_text(json.dumps(cfg))
            output = root / 'report'
            code = runner.main(['run', '--config', str(settings), '--profile', 'amd', '--mode', 'probe', '--sample-percent', '1', '--no-full-scenes', '--out', str(output)])
            result = json.loads((output / 'results.json').read_text())
            self.assertEqual(code, 0, result['summary'])
            self.assertEqual(set(result['capabilities']), {'storm', 'vk', 'vkr'})
            for name, capability in result['capabilities'].items():
                self.assertEqual(capability['status'], 'ok', (name, capability))
            case = result['cases'][0]
            for name, render in case['renders'].items():
                self.assertEqual(render['status'], 'ok', (name, render))
                for attempt in render['attempts']:
                    self.assertEqual((attempt['image_info']['width'], attempt['image_info']['height']), (96, 64))
            self.assertIn('ray_query', case['renders']['vkr']['stats']['backend'])
            self.assertFalse(case['renders']['vkr']['stats']['fallback'])
            self.assertIn('AMD', case['renders']['vk']['stats']['device'])
            self.assertEqual(len(case['comparisons']), 2)
            self.assertEqual(json.loads((output / 'failed_ids.json').read_text()), [])


if __name__ == '__main__':
    unittest.main()
