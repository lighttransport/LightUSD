"""Harness correctness tests; real renderer checks are opt-in integration tests."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

import config
import runner
from images import compare, gate
from report import report
from supervisor import execute, parse_stats


class RunnerTests(unittest.TestCase):
    def test_sample_is_reproducible_and_stratified(self):
        rows = [{'id': str(i), 'bytes': 10, 'dataset': 'test', 'group': 'geometry' if i < 100 else 'composition'} for i in range(200)]
        selected = config.sample(rows, 5, 7)
        self.assertEqual(selected, config.sample(list(reversed(rows)), 5, 7))
        self.assertEqual(len(selected), 10)
        self.assertEqual({r['group'] for r in selected}, {'geometry', 'composition'})

    def test_probe_bounds(self):
        for value in (0, 11, float('nan')):
            with self.assertRaises(ValueError):
                config.sample([], value, 0)

    def test_inventory_filters_fragments_and_roots(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'root.usda').write_text('#usda 1.0\n')
            (root / '._root.usda').write_text('resource fork')
            (root / 'model.xml').write_text('<mujoco><worldbody/></mujoco>')
            (root / 'fragment.xml').write_text('<mujoco><actuator/></mujoco>')
            cfg = {'datasets': [{'name': 'test', 'root': directory, 'kind': 'menagerie', 'full_scene_roots': ['root.usda'], 'full_variants': [{'set': 'lod', 'selection': 'full'}]}]}
            rows, datasets = config.inventory(cfg)
            self.assertEqual(len(rows), 3)
            self.assertEqual(sum(bool(r.get('full_scene')) for r in rows), 2)
            self.assertEqual(sum(r.get('input_kind') == 'mjcf' for r in rows), 1)
            self.assertEqual(datasets[0]['excluded']['xml_fragment'], 1)
            self.assertEqual(datasets[0]['excluded']['appledouble'], 1)

    def test_duplicate_ids_rejected(self):
        with self.assertRaises(ValueError):
            config.inventory({'cases': [{'id': 'same', 'path': '/nonexistent'}, {'id': 'same', 'path': '/nonexistent'}]})

    def test_config_paths_and_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / 'config.json'
            p.write_text(json.dumps({'datasets': [{'name': 'test', 'root': 'assets'}], 'renderers': []}))
            cfg = config.load(p)
            self.assertEqual(cfg['datasets'][0]['root'], str(p.parent / 'assets'))
            p.write_text(json.dumps({'renderers': [{'name': '../escape', 'argv': ['true']}]}))
            with self.assertRaises(ValueError):
                config.load(p)

    def test_image_metrics_and_alpha(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            Image.new('RGB', (16, 16), 'black').save(root / 'a.png')
            Image.new('RGB', (16, 16), 'white').save(root / 'b.png')
            metrics = compare(root / 'a.png', root / 'b.png', root / 'diff')
            self.assertEqual(metrics['normalized_rmse'], 1)
            self.assertEqual(metrics['changed_pixel_fraction'], 1)
            same = compare(root / 'a.png', root / 'a.png', root / 'same')
            self.assertEqual(same['ssim'], 1)
            self.assertTrue(same['psnr_infinite'])
            self.assertEqual(gate(same, {'psnr_db': {'min': 30}})['status'], 'pass')
            self.assertEqual(gate(metrics, {'normalized_rmse': {'max': 0.1}})['status'], 'fail')
            Image.new('RGBA', (16, 16), (255, 255, 255, 0)).save(root / 'alpha.png')
            alpha = compare(root / 'a.png', root / 'alpha.png', root / 'alpha')
            self.assertEqual(alpha['normalized_rmse'], 0)
            self.assertEqual(alpha['alpha_mae'], 1)
            self.assertFalse(alpha['identical'])
            Image.new('RGB', (8, 8)).save(root / 'small.png')
            with self.assertRaises(ValueError):
                compare(root / 'small.png', root / 'a.png', root / 'bad')

    def test_gate_rejects_unknown_or_invalid_thresholds(self):
        for thresholds in ({'bogus': {'max': 1}}, {'ssim': {'lt': 0.5}}, {'ssim': {'min': float('inf')}}):
            with self.assertRaises(ValueError):
                gate({}, thresholds)

    @unittest.skipUnless(sys.platform == 'linux', 'Linux /proc supervisor')
    def test_timeout_kills_descendants_and_records_rss(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = 'import subprocess,time,pathlib; p=subprocess.Popen(["sleep","20"]); pathlib.Path(%r).write_text(str(p.pid)); time.sleep(20)' % str(root / 'pid')
            result = execute([sys.executable, '-c', script], root, 0.3, max_rss_mib=512, reserve_mib=0)
            self.assertEqual(result['status'], 'timeout')
            pid = int((root / 'pid').read_text())
            status = Path('/proc/%d/status' % pid)
            try:
                state = status.read_text()
            except (FileNotFoundError, ProcessLookupError):
                state = ''
            self.assertTrue(not state or 'State:\tZ' in state)
            self.assertGreater(result['peak_tree_rss_bytes'], 0)

    @unittest.skipUnless(sys.platform == 'linux', 'Linux /proc supervisor')
    def test_process_rss_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            result = execute([sys.executable, '-c', 'a=bytearray(8*1024*1024)'], Path(directory) / 'ok', 5, max_rss_mib=512, reserve_mib=0)
            self.assertEqual(result['status'], 'ok')
            self.assertGreater(result['peak_rss_bytes'], 8 * 1024**2)
            result = execute([sys.executable, '-c', 'raise SystemExit(3)'], Path(directory) / 'bad', 5, max_rss_mib=512, reserve_mib=0)
            self.assertEqual(result['status'], 'render_error')
            self.assertEqual(result['returncode'], 3)

    @unittest.skipUnless(sys.platform == 'linux', 'Linux /proc supervisor')
    def test_memory_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            result = execute([sys.executable, '-c', 'import time; a=bytearray(80*1024*1024); time.sleep(5)'], directory, 5, max_rss_mib=40, reserve_mib=0)
            self.assertEqual(result['status'], 'resource_limit')

    def test_gpu_counter_budget_and_unavailable_are_not_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);counter=root/'vram';counter.write_text(str(2*1024**2))
            result=execute([sys.executable,'-c','raise SystemExit(99)'],root/'over',5,
                           max_rss_mib=512,reserve_mib=0,gpu_vram_path=counter,gpu_budget_mib=1)
            self.assertEqual(result['status'],'resource_limit')
            self.assertIsNone(result['returncode'])
            self.assertEqual(result['peak_device_vram_bytes'],2*1024**2)
            counter.unlink()
            result=execute([sys.executable,'-V'],root/'missing',5,
                           gpu_vram_path=counter,gpu_budget_mib=1)
            self.assertEqual(result['status'],'unavailable')
            self.assertIsNone(result['returncode'])

    def test_report_escapes_html_and_needs_no_fetch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'report.html'
            report({'cases': [{'id': '</script><script>alert(1)</script>', 'status': 'pass'}]}, path)
            content = path.read_text()
            self.assertNotIn('</script><script>alert(1)', content)
            self.assertIn('\\u003c/script>', content)
            self.assertNotIn('fetch(', content)
            self.assertIn("loading='lazy'", content)

    def test_dependency_change_invalidates_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'asset'
            path.write_text('one')
            dependencies = [config.metadata(path)]
            self.assertTrue(runner.dependencies_current(dependencies))
            path.write_text('different')
            self.assertFalse(runner.dependencies_current(dependencies))

    def test_backend_identity_and_fallback(self):
        self.assertEqual(parse_stats('backend: shared CUDA RT (NVIDIA GPU)')['device'], 'NVIDIA GPU')
        self.assertEqual(parse_stats('HIP device: AMD GPU')['device'], 'AMD GPU')
        self.assertEqual(parse_stats('Vulkan device: AMD Radeon RX 9070 XT (RADV GFX1201) (WARNING: loader warning')['device'], 'AMD Radeon RX 9070 XT (RADV GFX1201)')
        self.assertTrue(parse_stats('Vulkan device: llvmpipe')['software_device'])
        self.assertTrue(parse_stats('backend: LightRT VK (compute trace, ray-query fallback)')['fallback'])

    def test_atomic_json_rejects_nan(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                runner.write(Path(directory) / 'bad.json', {'value': float('nan')})

    def test_cli_comparison_threshold_exit_status(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            Image.new('RGB', (8, 8), 'black').save(root / 'a.png')
            Image.new('RGB', (8, 8), 'white').save(root / 'b.png')
            (root / 'thresholds.json').write_text('{"normalized_rmse":{"max":0.1}}')
            self.assertEqual(runner.main(['compare', '--reference', str(root / 'a.png'), '--candidate', str(root / 'b.png'), '--out', str(root / 'diff'), '--thresholds', str(root / 'thresholds.json')]), 1)


if __name__ == '__main__':
    unittest.main()
