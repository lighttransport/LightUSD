"""Failure triage, AMD profile and targeted rerun regression tests."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import config
from diagnostics import analyze_log, annotate_execution, failure_codes, hardware_error
import runner


class DiagnosticsTests(unittest.TestCase):
    def test_shader_errors_and_bounded_examples(self):
        text = "Could not find a nodedef for node 'Surface'\n" * 100
        text += "Node interface error: Input 'base_color' doesn't match declaration\n"
        findings = {entry['code']: entry for entry in analyze_log(text)}
        self.assertEqual(findings['materialx_nodedef_missing']['occurrences'], 100)
        self.assertEqual(len(findings['materialx_nodedef_missing']['examples']), 1)
        self.assertIn('materialx_interface_mismatch', findings)

    def test_log_read_is_bounded_and_classifies_context_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'stderr.log'
            log.write_text('No renderable geometry\n' + 'x' * (512 * 1024) + '\nqt.qpa.xcb: could not connect to display :0\n')
            row = annotate_execution({'status': 'render_error', 'stderr': str(log)})
            self.assertEqual([d['code'] for d in row['diagnostics']], ['graphics_context_unavailable'])

    def test_explicit_device_and_ray_query_requirements(self):
        renderer = {'kind': 'lusdrender', 'require_hardware': True, 'device_pattern': 'AMD.*Radeon', 'backend_pattern': 'ray_query'}
        stats = {'device': 'AMD Radeon RX 9070 XT', 'backend': 'LightRT VK (ray_query)'}
        self.assertIsNone(hardware_error(renderer, stats))
        self.assertEqual(hardware_error(renderer, dict(stats, device='NVIDIA GPU'))[0], 'device_mismatch')
        self.assertEqual(hardware_error(renderer, dict(stats, backend='compute trace'))[0], 'backend_mismatch')
        self.assertEqual(hardware_error(renderer, dict(stats, fallback=True))[0], 'backend_fallback')
        self.assertEqual(hardware_error(renderer, dict(stats, software_device=True))[0], 'software_device')
        storm = {'kind': 'usdrecord', 'require_hardware': True, 'device_pattern': 'AMD.*Radeon'}
        self.assertIsNone(hardware_error(storm, {}, 'OpenGL renderer string: AMD Radeon RX 9070 XT'))
        self.assertEqual(hardware_error(storm, {}, 'OpenGL renderer string: Other GPU')[0], 'device_mismatch')

    def test_empty_renderer_geometry_is_not_a_device_failure(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def fake_command(argv, output, cfg, env):
                output.mkdir(parents=True, exist_ok=True)
                Image.new('RGB', (16, 16)).save(output / 'image.png')
                log = output / 'stderr.log'
                log.write_text('WARN: No renderable geometry found; writing blank image.\n')
                return annotate_execution({'status': 'ok', 'stderr': str(log)})
            renderer = {'name': 'vk', 'kind': 'lusdrender', 'executable': 'unused', 'backend': 'vk', 'require_hardware': True}
            prepared = {'scene': 'unused', 'frame': 'default', 'width': 16, 'height': 16, 'camera': '/Camera', 'purposes': ['default']}
            with patch.object(runner, 'run_command', side_effect=fake_command):
                result = runner.render_once(renderer, prepared, {}, root, {}, SimpleNamespace(env={}))
            self.assertEqual(result['status'], 'no_renderable_geometry')
            self.assertEqual(result['diagnostics'][0]['code'], 'no_renderable_geometry')

    def test_failure_counts_keep_nonfatal_warnings_informational(self):
        data = {'cases': [
            {'id': 'failed', 'status': 'fail', 'renders': {'vk': {'status': 'no_renderable_geometry', 'diagnostics': [{'code': 'no_renderable_geometry'}]}}},
            {'id': 'good', 'status': 'pass', 'renders': {'storm': {'status': 'ok', 'diagnostics': [{'code': 'materialx_interface_mismatch'}]}}},
            {'id': 'expected', 'status': 'xfail'}],
            'selected_count': 3, 'inventory_count': 3, 'capabilities': {}}
        data['cases'][0]['failure_codes'] = failure_codes(data['cases'][0])
        runner.summarize(data)
        self.assertEqual(data['failed_ids'], ['failed'])
        self.assertEqual(data['summary']['failure_code_counts'], {'no_renderable_geometry': 1})

    def test_failed_attempt_keeps_time_memory_and_available_image(self):
        attempts = [{'status': 'ok', 'wall_s': 1, 'peak_rss_bytes': 100, 'image': 'first.png'},
                    {'status': 'resource_limit', 'wall_s': 3, 'peak_rss_bytes': None, 'peak_tree_rss_bytes': 1000}]
        summary = runner.summarize_attempts(attempts, 2)
        self.assertEqual(summary['status'], 'resource_limit')
        self.assertEqual(summary['wall_s'], 3)
        self.assertEqual(summary['peak_tree_rss_bytes'], 1000)
        self.assertIsNone(summary['peak_rss_bytes'])
        self.assertEqual(summary['total_wall_s'], 4)
        self.assertEqual(summary['measurement_kind'], 'failed_attempt')
        self.assertEqual(summary['image_attempt'], 0)
        self.assertEqual(summary['image'], 'first.png')

    def test_successful_summary_excludes_warmups_from_medians(self):
        attempts = [{'status': 'ok', 'warmup': True, 'wall_s': 10, 'peak_rss_bytes': 1000},
                    {'status': 'ok', 'wall_s': 1, 'peak_rss_bytes': 100},
                    {'status': 'ok', 'wall_s': 3, 'peak_rss_bytes': 200}]
        summary = runner.summarize_attempts(attempts, 2)
        self.assertEqual(summary['wall_s'], 2)
        self.assertEqual(summary['peak_rss_bytes'], 150)
        self.assertEqual(summary['total_wall_s'], 14)
        self.assertEqual(summary['measurement_count'], 2)
        self.assertEqual(summary['status'], 'ok')

    def test_profile_selection_and_validation(self):
        cfg = config.load(config.HERE / 'presets.json', profile='amd')
        self.assertEqual([r['name'] for r in cfg['renderers']], ['storm', 'vk', 'vkr'])
        self.assertEqual(cfg['renderers'][-1]['backend_pattern'], 'ray_query')
        self.assertTrue(all(r['require_hardware'] for r in cfg['renderers']))
        self.assertEqual(cfg['_profile'], 'amd')
        with self.assertRaises(ValueError):
            config.load(config.HERE / 'presets.json', profile='unknown')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'config.json'
            path.write_text(json.dumps({'renderers': [{'name': 'custom', 'argv': ['true'], 'device_pattern': '['}]}))
            with self.assertRaises(ValueError):
                config.load(path)

    def test_profile_merges_settings_and_resolves_paths_at_config(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'config.json'
            path.write_text(json.dumps({'render': {'width': 64, 'height': 32}, 'profiles': {'small': {'render': {'width': 32}, 'cases': [{'id': 'a', 'path': 'asset.usda'}]}}}))
            cfg = config.load(path, profile='small')
            self.assertEqual(cfg['render'], {'width': 32, 'height': 32})
            self.assertEqual(cfg['cases'][0]['path'], str(path.parent / 'asset.usda'))

    def test_failed_frame_selection_roundtrips_without_mutating_inventory(self):
        eligible = [{'id': 'a', 'path': 'a.usda', 'render': {'camera': '/Camera'}}]
        original = copy.deepcopy(eligible)
        selected = runner.requested_cases(eligible, ['a#frame=2.5', 'a#frame=default', 'a#frame=2.5'])
        self.assertEqual(len(selected), 2)
        self.assertEqual(selected[0]['render']['frame'], 2.5)
        self.assertEqual(selected[0]['source_id'], 'a')
        self.assertEqual(selected[1]['render']['frame'], 'default')
        self.assertEqual(eligible, original)
        with self.assertRaises(ValueError):
            runner.requested_cases(eligible, ['excluded/root#frame=1'])
        with self.assertRaises(ValueError):
            runner.requested_cases(eligible, ['a#frame=nan'])

    def test_failed_ids_exclude_expected_and_empty_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'results.json'
            path.write_text(json.dumps({'cases': [{'id': 'a', 'status': 'fail'}, {'id': 'b', 'status': 'xfail'}, {'id': 'c', 'status': 'support_or_empty'}, {'id': 'd#frame=2', 'status': 'preparation_error'}]}))
            self.assertEqual(runner.rerun_ids(path, True), ['a', 'd#frame=2'])

    def test_clean_failed_run_does_not_launch_renderers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scene = root / 'asset.usda'
            scene.write_text('#usda 1.0\n')
            cfg = root / 'config.json'
            cfg.write_text(json.dumps({'cases': [{'id': 'a', 'path': str(scene)}], 'renderers': [{'name': 'unused', 'argv': [sys.executable]}]}))
            previous = root / 'clean.json'
            previous.write_text('{"cases":[{"id":"a","status":"pass"}]}')
            with patch.object(runner, 'capability_checks', side_effect=AssertionError('must not launch')):
                code = runner.main(['run', '--config', str(cfg), '--failed-from', str(previous), '--out', str(root / 'out')])
            self.assertEqual(code, 0)
            self.assertEqual(json.loads((root / 'out/failed_ids.json').read_text()), [])
            self.assertEqual(json.loads((root / 'out/results.json').read_text())['summary']['status'], 'no_failures_to_rerun')


if __name__ == '__main__':
    unittest.main()
