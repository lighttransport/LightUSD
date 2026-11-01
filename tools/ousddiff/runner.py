#!/usr/bin/env python3
"""Reproducible, resource-bounded OpenUSD / lusdrender offscreen comparisons."""
import argparse
from collections import Counter
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

from config import HERE, REPO, digest, inventory, load, metadata, sample
from images import compare, gate, validate
from report import report
from supervisor import execute, parse_stats


def write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(data, indent=2, allow_nan=False))
    temporary.replace(path)


def file_hash(path):
    try:
        with open(path, 'rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest()
    except OSError:
        return None


def openusd_env(config):
    env = dict(os.environ)
    dist = config.get('openusd', {}).get('dist')
    if dist:
        for name, suffix in [('PYTHONPATH', 'lib/python'), ('LD_LIBRARY_PATH', 'lib'), ('PXR_PLUGINPATH_NAME', 'plugin/usd')]:
            env[name] = str(Path(dist) / suffix) + (os.pathsep + env[name] if env.get(name) else '')
    return env


def limits(config):
    return {key: config['resources'][key] for key in ('timeout_s', 'max_rss_mib', 'reserve_mib')}


def run_command(argv, directory, config, env=None):
    policy = limits(config)
    return execute(argv, directory, policy.pop('timeout_s'), env, **policy)


def prepare_case(row, config, directory):
    render = dict(config.get('render', {}), **row.get('render', {}))
    scene = row['path']
    stages = {}
    if row.get('inventory_error'):
        return {'status': 'preparation_error', 'error': row['inventory_error']}, stages
    if row.get('input_kind') == 'mjcf':
        converted = directory / 'converted' / 'model.usdc'
        converted.parent.mkdir(parents=True, exist_ok=True)
        if not config.get('converter'):
            return {'status': 'preparation_error', 'error': 'Menagerie requires converter'}, stages
        argv = [config['converter'], scene, '--input-format', 'mjcf', '--format', 'usdc', '-o', str(converted), '--asset-dir', row['root'], '--max-memory', str(int(config['resources']['max_rss_mib'] * 1024**2))]
        stages['conversion'] = run_command(argv, directory / 'conversion', config)
        if stages['conversion']['status'] != 'ok' or not converted.is_file():
            return {'status': 'conversion_error', 'error': 'MJCF conversion failed'}, stages
        scene = str(converted)
    if config.get('prepare', True) is False:
        return {'status': 'ok', 'scene': scene, 'classification': 'uninspected', **render, 'dependencies': [metadata(row['path'])]}, stages
    interpreter = config.get('openusd', {}).get('python')
    if not interpreter:
        return {'status': 'preparation_error', 'error': 'openusd.python is required for shared input preparation'}, stages
    request = {'scene': scene, 'directory': str(directory / 'prepared'), 'render': render, 'variant_rules': row.get('variant_rules', [])}
    request_path, response = directory / 'prepare-request.json', directory / 'prepared.json'
    write(request_path, request)
    stages['preparation'] = run_command([interpreter, str(HERE / 'prepare.py'), str(request_path), str(response)], directory / 'preparation', config, openusd_env(config))
    if response.is_file():
        prepared = json.loads(response.read_text())
    else:
        prepared = {'status': 'preparation_error', 'error': 'preparation worker produced no response'}
    if stages['preparation']['status'] != 'ok':
        prepared['status'] = stages['preparation']['status'] if stages['preparation']['status'] in ('timeout', 'resource_limit') else 'preparation_error'
    return prepared, stages


def argv_for(renderer, prepared, image, settings):
    kind = renderer.get('kind', 'custom')
    replacements = {'scene': prepared['scene'], 'image': str(image), 'width': str(prepared.get('width', 512)), 'height': str(prepared.get('height', 512)), 'camera': prepared.get('camera', ''), 'frame': str(prepared.get('frame', 0))}
    if kind == 'custom':
        argv = list(renderer['argv'])
        for key, value in replacements.items():
            argv = [v.replace('{' + key + '}', value) for v in argv]
        return argv
    frame = prepared['frame']
    if kind == 'usdrecord':
        argv = [renderer['executable'], '--renderer', renderer.get('backend', 'Embree'), '--imageWidth', str(prepared['width']), '--camera', prepared['camera'], '--complexity', settings.get('complexity', 'low'), '--purposes', ','.join(p for p in prepared['purposes'] if p != 'default'), '--disableDrawMode']
        if renderer.get('backend', 'Embree') == 'Embree':
            argv += ['--disableGpu', '--colorCorrectionMode', 'disabled']
        else:
            argv += ['--colorCorrectionMode', 'sRGB']
        if frame == 'default':
            argv += ['--defaultTime']
        else:
            # usdrecord requires a frame placeholder even for an explicit single frame.
            argv += ['--frames', str(frame)]
        argv += renderer.get('args', []) + [prepared['scene'], str(image)]
    else:
        backend = renderer.get('backend', 'cpu')
        flag = '-rtPreview' if backend == 'cpu' else '-' + backend
        argv = [renderer['executable'], prepared['scene'], str(image), flag, '-stats', '-w', str(prepared['width']), '-height', str(prepared['height']), '-camera', prepared['camera'], '-purpose', ','.join(prepared['purposes']), '-complexity', settings.get('complexity', 'low')]
        argv += ['-defaultTime'] if frame == 'default' else ['-timecode', str(frame)]
        if settings.get('threads') is not None:
            argv += ['-threads', str(settings['threads'])]
        argv += renderer.get('args', [])
    return argv


class Display:
    """One Xvfb per batch; startup does not pollute individual render timings."""
    def __init__(self, output, needed):
        self.process = None
        self.env = {}
        self.error = None
        self.log = None
        if not needed:
            return
        self.env['QT_QPA_PLATFORM'] = 'xcb'
        if os.environ.get('DISPLAY'):
            return
        if not shutil.which('Xvfb'):
            self.error = 'Storm requires DISPLAY or Xvfb'
            return
        import select
        read_fd, write_fd = os.pipe()
        self.log = open(output / 'xvfb.log', 'w')
        try:
            self.process = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1280x800x24', '-nolisten', 'tcp'], pass_fds=(write_fd,), stdout=self.log, stderr=self.log, start_new_session=True)
            os.close(write_fd)
            write_fd = None
            if select.select([read_fd], [], [], 5)[0]:
                number = os.read(read_fd, 32).decode().strip()
                if number.isdigit():
                    self.env['DISPLAY'] = ':' + number
                else:
                    self.error = 'Xvfb did not provide a display number'
            else:
                self.error = 'Xvfb display startup timed out'
        except OSError as exc:
            self.error = str(exc)
        finally:
            os.close(read_fd)
            if write_fd is not None:
                os.close(write_fd)

    def close(self):
        if self.process:
            from supervisor import guard
            guard.stop_group(self.process, grace=0.2)
        if self.log:
            self.log.close()


def render_once(renderer, prepared, settings, directory, config, display):
    directory.mkdir(parents=True, exist_ok=True)
    # Render each numeric time in a separate process, but satisfy usdrecord's
    # filename formatting contract and rename its actual output deterministically.
    image = directory / 'image.png'
    requested = image
    if renderer.get('kind') == 'usdrecord' and prepared.get('frame') != 'default':
        requested = directory / 'image.####.###.png'
    argv = argv_for(renderer, prepared, requested, settings)
    env = openusd_env(config) if renderer.get('kind') == 'usdrecord' else dict(os.environ)
    env.update(display.env)
    env.update(renderer.get('env', {}))
    row = run_command(argv, directory, config, env)
    if row['status'] == 'ok':
        if requested != image:
            outputs = sorted(directory.glob('image.*.png'))
            if len(outputs) == 1:
                outputs[0].replace(image)
        try:
            row['image_info'] = validate(image, [prepared['width'], prepared['height']] if config.get('prepare', True) else None)
            row['image'] = str(image)
        except (OSError, ValueError) as exc:
            row.update(status='invalid_image', error=str(exc))
    try:
        log = Path(row['stderr']).read_text(errors='replace')
        stats = parse_stats(log)
        row['stats'] = stats
        if renderer.get('require_hardware') and (stats['software_device'] or stats['fallback']):
            row.update(status='backend_mismatch', error='software device or backend fallback rejected')
        if renderer.get('kind') == 'lusdrender' and renderer.get('require_hardware') and not stats.get('device') and row['status'] == 'ok':
            row.update(status='backend_mismatch', error='hardware identity missing from renderer output')
    except OSError:
        pass
    return row


def capability_checks(config, output, display):
    capabilities = {}
    fixture = output / 'capability.usda'
    fixture.write_text('''#usda 1.0
(upAxis = "Y")
def Mesh "Triangle" {
 point3f[] points = [(-1,-1,0),(1,-1,0),(0,1,0)]
 int[] faceVertexCounts = [3]
 int[] faceVertexIndices = [0,1,2]
 uniform token subdivisionScheme = "none"
}
def Camera "Camera" {
 float horizontalAperture = 20
 float verticalAperture = 20
 float focalLength = 50
 double3 xformOp:translate = (0,0,10)
 uniform token[] xformOpOrder = ["xformOp:translate"]
}
''')
    prepared = {'scene': str(fixture), 'camera': '/Camera', 'frame': 'default', 'width': 32, 'height': 32, 'purposes': ['default', 'render']}
    for renderer in config['renderers']:
        if renderer.get('kind') == 'usdrecord' and renderer.get('backend') == 'Storm' and display.error:
            row = {'status': 'unavailable', 'error': display.error}
        elif renderer.get('kind', 'custom') == 'custom':
            executable_path = renderer['argv'][0]
            row = {'status': 'ok' if shutil.which(executable_path) else 'unavailable', 'note': 'custom renderer: executable availability only'}
        else:
            probe_config = copy.deepcopy(config)
            probe_config['resources']['timeout_s'] = min(45, config['resources']['timeout_s'])
            row = render_once(renderer, prepared, {'complexity': 'low'}, output / 'capabilities' / renderer['name'], probe_config, display)
            if renderer.get('require_hardware') and renderer.get('kind') == 'usdrecord' and row['status'] == 'ok':
                if not shutil.which('glxinfo'):
                    row.update(status='unavailable', error='glxinfo required to verify Storm hardware identity')
                else:
                    env = dict(os.environ, **display.env, **renderer.get('env', {}))
                    identity = run_command(['glxinfo', '-B'], output / 'capabilities' / renderer['name'] / 'identity', probe_config, env)
                    text = Path(identity['stdout']).read_text(errors='replace') if Path(identity['stdout']).is_file() else ''
                    row['hardware_identity'] = '\n'.join(line.strip() for line in text.splitlines() if any(key in line for key in ('OpenGL vendor string:', 'OpenGL renderer string:', 'OpenGL core profile version string:', 'direct rendering:')))
                    if identity['status'] != 'ok' or 'OpenGL renderer string:' not in text or parse_stats(text)['software_device']:
                        row.update(status='backend_mismatch', error='Storm hardware identity unavailable or software renderer')
        row['required'] = renderer.get('required', True)
        capabilities[renderer['name']] = row
    return capabilities


def dependencies_current(dependencies):
    for old in dependencies:
        try:
            if metadata(old['path']) != old:
                return False
        except OSError:
            return False
    return True


def historical(case, baseline, config, directory, compatible_host):
    old = baseline.get(case['id'])
    if not old or old.get('render_key') != case.get('render_key') or not dependencies_current(old.get('dependencies', [])) or not compatible_host:
        return {'status': 'baseline_incompatible', 'reason': 'missing case, changed inputs/settings, or changed platform/device identity'}
    entries = {}
    failed = False
    image_thresholds = config.get('historical_thresholds', {'normalized_rmse': {'max': 0.01}, 'ssim': {'min': 0.99}})
    for name, render in case['renders'].items():
        previous = old.get('renders', {}).get(name, {})
        if render.get('status') != 'ok' or previous.get('status') != 'ok':
            entries[name] = {'status': 'baseline_incompatible'}
            failed = True
            continue
        try:
            metrics = compare(previous['image'], render['image'], directory / name)
            entry = {'metrics': metrics, 'visual': gate(metrics, image_thresholds), 'performance': {}}
            failed |= entry['visual']['status'] == 'fail'
            for field in ('wall_s', 'peak_rss_bytes', 'peak_tree_rss_bytes'):
                before, after = previous.get(field), render.get(field)
                if before is None or after is None:
                    continue
                relative = after / before - 1 if before else None
                policy = config.get('performance_thresholds', {}).get(field)
                violated = False
                if policy and relative is not None:
                    violated = relative > policy.get('relative', 0) and after - before > policy.get('absolute', 0)
                entry['performance'][field] = {'before': before, 'after': after, 'relative_change': relative, 'absolute_change': after - before, 'status': 'fail' if violated else ('pass' if policy else 'informational')}
                failed |= violated
            entries[name] = entry
        except (OSError, ValueError) as exc:
            entries[name] = {'status': 'baseline_incompatible', 'error': str(exc)}
            failed = True
    return {'status': 'fail' if failed else 'pass', 'renderers': entries}


def case_run(row, config, output, capabilities, display):
    directory = output / 'cases' / digest(row['id'])[:24]
    directory.mkdir(parents=True, exist_ok=True)
    checkpoint = directory / 'result.json'
    render_config = dict(config.get('render', {}), **row.get('render', {}))
    # Hash renderer arguments/settings, but deliberately not executable content:
    # comparing a changed implementation against history is the purpose.
    key = digest({'input': row.get('fingerprint'), 'variants': row.get('variant_rules', []), 'render': render_config,
                  'renderers': config['renderers'], 'prepare': config.get('prepare', True)})
    resume_key = digest({'render_key': key, 'binaries': config.get('_binary_hashes'), 'runner': {p.name: file_hash(p) for p in HERE.glob('*.py')}, 'capabilities': platform_identity(capabilities), 'thresholds': config.get('thresholds'), 'pairs': config.get('pairs'), 'case_thresholds': row.get('thresholds')})
    if config.get('_resume') and checkpoint.exists():
        cached = json.loads(checkpoint.read_text())
        if cached.get('resume_key') == resume_key and dependencies_current(cached.get('dependencies', [])) and cached.get('status') in ('pass', 'support_or_empty', 'empty_for_purposes') and all(Path(r['image']).is_file() for r in cached.get('renders', {}).values() if r.get('status') == 'ok'):
            cached['resumed'] = True
            return cached
    # Remove stale output so an unsuccessful command cannot inherit an old image.
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    case = dict(row, render_key=key, resume_key=resume_key, renders={}, comparisons=[], status='pass')
    prepared, stages = prepare_case(row, config, directory)
    case.update(prepared=prepared, stages=stages, dependencies=prepared.get('dependencies', []))
    if prepared['status'] != 'ok':
        case['status'] = prepared['status']
    elif prepared['classification'] in ('support_or_empty', 'empty_for_purposes'):
        case['status'] = prepared['classification']
    else:
        for renderer in config['renderers']:
            name = renderer['name']
            capability = capabilities[name]
            if capability['status'] != 'ok':
                case['renders'][name] = {'status': 'unavailable', 'reason': capability.get('error', capability['status']), 'required': renderer.get('required', True)}
                if renderer.get('required', True):
                    case['status'] = 'fail'
                continue
            attempts = []
            for iteration in range(render_config.get('warmups', 0) + render_config.get('repeat', 1)):
                attempt = render_once(renderer, prepared, render_config, directory / name / str(iteration), config, display)
                attempt['warmup'] = iteration < render_config.get('warmups', 0)
                attempts.append(attempt)
                if attempt['status'] != 'ok':
                    break
            measured = [a for a in attempts if not a['warmup'] and a['status'] == 'ok']
            render = {'status': 'ok' if len(measured) == render_config.get('repeat', 1) and all(a['status'] == 'ok' for a in attempts) else 'render_error', 'attempts': attempts}
            if measured:
                render['image'] = measured[-1]['image']
                render['stats'] = measured[-1].get('stats', {})
                for field in ('wall_s', 'peak_rss_bytes', 'peak_tree_rss_bytes'):
                    values = [a[field] for a in measured if a.get(field) is not None]
                    render[field] = statistics.median(values) if values else None
                render['measurement_count'] = len(measured)
            case['renders'][name] = render
            if render['status'] != 'ok':
                case['status'] = 'fail'
        for index, pair in enumerate(config.get('pairs', [])):
            policy = pair if isinstance(pair, dict) else {'renderers': pair}
            members = policy['renderers']
            item = {'renderers': members}
            if any(case['renders'][name]['status'] != 'ok' for name in members):
                item['status'] = 'not_comparable'
            else:
                try:
                    a, b = (case['renders'][name] for name in members)
                    metrics = compare(a['image'], b['image'], directory / ('pair-' + str(index)), config.get('pixel_tolerance', 8))
                    thresholds = dict(config.get('thresholds', {}), **policy.get('thresholds', {}), **row.get('thresholds', {}))
                    if 'max_normalized_rmse' in config:
                        thresholds.setdefault('normalized_rmse', {'max': config['max_normalized_rmse']})
                    item.update(metrics=metrics, **gate(metrics, thresholds))
                    item['wall_ratio_reference_over_candidate'] = a['wall_s'] / b['wall_s'] if b.get('wall_s') else None
                    if item['status'] == 'fail':
                        case['status'] = 'fail'
                except (OSError, ValueError) as exc:
                    item.update(status='invalid_comparison', error=str(exc))
                    case['status'] = 'fail'
            case['comparisons'].append(item)
    write(checkpoint, case)
    return case


def platform_identity(capabilities):
    return {name: {'device': cap.get('stats', {}).get('device'), 'backend': cap.get('stats', {}).get('backend'), 'hardware_identity': cap.get('hardware_identity'), 'status': cap['status']} for name, cap in capabilities.items()}


def summarize(result):
    counts = Counter(case['status'] for case in result['cases'])
    result['summary'] = {'status_counts': dict(counts), 'completed': len(result['cases']),
                         'selected': result['selected_count'], 'inventory': result['inventory_count'],
                         'capability_failures': [name for name, cap in result['capabilities'].items() if cap['status'] != 'ok' and cap['required']],
                         'datasets': {}}
    for dataset in {case.get('dataset', 'explicit') for case in result['cases']}:
        cases = [c for c in result['cases'] if c.get('dataset', 'explicit') == dataset]
        result['summary']['datasets'][dataset] = dict(Counter(c['status'] for c in cases))
    success = {'pass', 'support_or_empty', 'empty_for_purposes', 'xfail'}
    result['summary']['comparison_status_counts'] = dict(Counter(p['status'] for c in result['cases'] for p in c.get('comparisons', [])))
    result['summary']['renderer_totals'] = {}
    for name in result['capabilities']:
        renders = [c.get('renders', {}).get(name, {}) for c in result['cases']]
        result['summary']['renderer_totals'][name] = {'completed': sum(r.get('status') == 'ok' for r in renders), 'sum_wall_s': sum(r.get('wall_s') or 0 for r in renders), 'max_peak_rss_bytes': max((r.get('peak_rss_bytes') or 0 for r in renders), default=0)}
    result['summary']['failed'] = bool(result['summary']['capability_failures']) or any(c['status'] not in success for c in result['cases']) or any(d.get('required', True) and not d['available'] for d in result.get('datasets', []))


def source_revision():
    try:
        return subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], capture_output=True, text=True, timeout=5).stdout.strip() or None
    except (OSError, subprocess.TimeoutExpired):
        return None


def driver_identities(config, output, capabilities):
    identities = {}
    for renderer in config['renderers']:
        name = renderer['name']
        if renderer.get('backend') not in ('vk', 'vkr') or capabilities[name]['status'] != 'ok':
            continue
        if not shutil.which('vulkaninfo'):
            identities[name] = {'status': 'unavailable', 'reason': 'vulkaninfo missing'}
            continue
        policy = copy.deepcopy(config)
        policy['resources']['timeout_s'] = min(15, config['resources']['timeout_s'])
        probe = run_command(['vulkaninfo', '--summary'], output / 'capabilities' / name / 'driver', policy, dict(os.environ, **renderer.get('env', {})))
        devices = []
        if probe['status'] == 'ok':
            for line in Path(probe['stdout']).read_text(errors='replace').splitlines():
                if line.startswith('GPU') and line.rstrip().endswith(':'):
                    devices.append({})
                elif devices and '=' in line:
                    key, value = (v.strip() for v in line.split('=', 1))
                    if key in ('apiVersion', 'driverVersion', 'driverID', 'driverName', 'driverInfo', 'vendorID', 'deviceID', 'deviceName'):
                        devices[-1][key] = value
        identities[name] = {'status': probe['status'], 'devices': devices}
    return identities


def cpu_identity():
    try:
        model = next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name'))
    except (OSError, StopIteration):
        model = None
    return {'model': model, 'logical_cores': os.cpu_count()}


def run(config, args):
    if args.max_input_mib is not None and (not math.isfinite(args.max_input_mib) or args.max_input_mib <= 0):
        raise ValueError('--max-input-mib must be positive and finite')
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if not config['renderers']:
        raise ValueError('run requires at least one renderer')
    rows, datasets = inventory(config)
    eligible = []
    excluded = Counter()
    for row in rows:
        reason = None
        if args.no_full_scenes and row.get('full_scene'):
            reason = 'full_scene'
        elif args.max_input_mib and row.get('bytes', 0) > args.max_input_mib * 1024**2:
            reason = 'input_size'
        elif args.group and row.get('group') not in args.group:
            reason = 'group'
        if reason:
            excluded[reason] += 1
        else:
            eligible.append(row)
    selected = sample(eligible, args.sample_percent, args.seed) if args.mode == 'probe' else eligible
    if args.cases_from:
        requested = json.loads(args.cases_from.read_text())
        if isinstance(requested, dict):
            requested = requested.get('selected_ids', [r['id'] for r in requested.get('cases', [])])
        wanted = set(requested)
        missing = wanted - {r['id'] for r in eligible}
        if missing:
            raise ValueError('requested cases missing or excluded: ' + ', '.join(sorted(missing)))
        selected = [r for r in eligible if r['id'] in wanted]
    selected_scene_count = len(selected)
    if args.frames:
        expanded = []
        for row in selected:
            for frame in args.frames:
                item = copy.deepcopy(row)
                item['id'] += '#frame=' + frame
                item['render'] = dict(item.get('render', {}), frame='default' if frame == 'default' else float(frame))
                expanded.append(item)
        selected = expanded
    config['_resume'] = args.resume
    config['_binary_hashes'] = {r['name']: file_hash(r.get('executable', r.get('argv', [''])[0])) for r in config['renderers']}
    selection = {'schema_version': 1, 'mode': args.mode, 'sample_percent': args.sample_percent if args.mode == 'probe' else None,
                 'seed': args.seed, 'selection_method': 'explicit_ids' if args.cases_from else ('stratified_hash_round_up' if args.mode == 'probe' else 'all'), 'selected_scene_count': selected_scene_count, 'effective_percent': 100 * selected_scene_count / len(eligible) if eligible else 0, 'inventory_count': len(rows), 'eligible_count': len(eligible),
                 'excluded': dict(excluded), 'selected_ids': [r['id'] for r in selected], 'cases': selected,
                 'strata': dict(Counter('%s/%s/10^%s' % (r.get('dataset'), r.get('group'), int(math.log10(max(1, r.get('bytes', 1))))) for r in selected))}
    write(output / 'selection.json', selection)
    result = {'schema_version': 1, 'mode': args.mode, 'inventory_count': len(rows), 'selected_count': len(selected),
              'eligible_count': len(eligible), 'selection': str(output / 'selection.json'), 'datasets': datasets,
              'seed': args.seed, 'config': config, 'cases': [], 'capabilities': {},
              'provenance': {'source_revision': source_revision(), 'python': sys.version, 'cpu': cpu_identity(), 'environment': {key: value for key, value in os.environ.items() if key.startswith(('HDEMBREE_', 'PXR_WORK_', 'OMP_', 'VK_', 'MESA_VK_', '__NV_')) or key in ('CUDA_VISIBLE_DEVICES', 'HIP_VISIBLE_DEVICES', 'ROCR_VISIBLE_DEVICES')}, 'platform': os.uname().sysname + ' ' + os.uname().release,
                             'renderers': {r['name']: {'executable_sha256': file_hash(r.get('executable', r.get('argv', [''])[0]))} for r in config['renderers']},
                             'converter_sha256': file_hash(config.get('converter', '')),
                             'runner_sha256': {p.name: file_hash(p) for p in HERE.glob('*.py')}}}
    if args.optional and (not selected or any(not d['available'] and d['required'] for d in datasets)):
        result['summary'] = {'failed': False, 'status': 'skipped', 'completed': 0}
        write(output / 'results.json', result)
        report(result, output / 'report.html')
        return 77
    if not selected:
        result['error'] = 'no eligible cases selected'
        result['summary'] = {'failed': True, 'completed': 0}
        write(output / 'results.json', result)
        report(result, output / 'report.html')
        return 1
    display = Display(output, any(r.get('kind') == 'usdrecord' and r.get('backend') == 'Storm' for r in config['renderers']))
    try:
        result['capabilities'] = capability_checks(config, output, display)
        result['provenance']['devices'] = platform_identity(result['capabilities'])
        result['provenance']['drivers'] = driver_identities(config, output, result['capabilities'])
        baseline_data = json.loads(args.baseline.read_text()) if args.baseline else None
        baseline = {case['id']: case for case in baseline_data['cases']} if baseline_data else {}
        compatible_host = not baseline_data or (baseline_data.get('provenance', {}).get('devices') == result['provenance']['devices'] and baseline_data.get('provenance', {}).get('platform') == result['provenance']['platform'] and baseline_data.get('provenance', {}).get('drivers') == result['provenance']['drivers'] and baseline_data.get('provenance', {}).get('cpu') == result['provenance']['cpu'] and baseline_data.get('provenance', {}).get('environment') == result['provenance']['environment'])
        for index, row in enumerate(selected):
            print('[%d/%d] %s' % (index + 1, len(selected), row['id']), flush=True)
            case = case_run(row, config, output, result['capabilities'], display)
            if args.baseline and case['status'] == 'pass':
                case['historical'] = historical(case, baseline, config, output / 'history' / digest(row['id'])[:24], compatible_host)
                if case['historical']['status'] != 'pass':
                    case['status'] = 'fail'
            expectation = row.get('expect') or config.get('expected_failures', {}).get(row['id'])
            if expectation:
                if not expectation.get('reason') or not expectation.get('status'):
                    raise ValueError('expected failures require status and reason')
                actual = case['status']
                case['expectation'] = expectation
                case['actual_status'] = actual
                case['status'] = 'xfail' if actual == expectation['status'] else ('xpass' if actual == 'pass' else actual)
            result['cases'].append(case)
            summarize(result)
            write(output / 'results.json', result)
            # Avoid quadratic HTML work for large batches; JSON is always current.
            if index == 0 or (index + 1) % 25 == 0:
                report(result, output / 'report.html')
        result['baseline_removed_ids'] = sorted(set(baseline) - {case['id'] for case in selected})
    except KeyboardInterrupt:
        result['interrupted'] = True
        summarize(result)
        result['summary']['failed'] = True
        write(output / 'results.json', result)
        report(result, output / 'report.html')
        return 130
    finally:
        display.close()
    summarize(result)
    write(output / 'results.json', result)
    report(result, output / 'report.html')
    print(json.dumps(result['summary'], indent=2))
    return int(result['summary']['failed'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest='command', required=True)
    for command in ('list', 'run'):
        sub = subparsers.add_parser(command)
        sub.add_argument('--config', type=Path, default=HERE / 'presets.json')
        sub.add_argument('--preset', help='comma-separated dataset names; default all')
        sub.add_argument('--out', type=Path, required=True)
        if command == 'run':
            sub.add_argument('--mode', choices=['probe', 'full'], default='probe')
            sub.add_argument('--sample-percent', type=float, default=5)
            sub.add_argument('--seed', type=int, default=0)
            sub.add_argument('--baseline', type=Path)
            sub.add_argument('--cases-from', type=Path)
            sub.add_argument('--resume', action='store_true')
            sub.add_argument('--optional', action='store_true', help='exit 77 if the external dataset is absent')
            sub.add_argument('--no-full-scenes', action='store_true', help='exclude preset complete scene roots before sampling')
            sub.add_argument('--max-input-mib', type=float)
            sub.add_argument('--group', action='append', choices=['geometry', 'composition', 'support_candidate'])
            sub.add_argument('--frames', nargs='+', help='explicit numeric times or default; one process per frame')
            sub.add_argument('--renderers', help='comma-separated renderer names')
    sub = subparsers.add_parser('compare')
    sub.add_argument('--reference', type=Path, required=True)
    sub.add_argument('--candidate', type=Path, required=True)
    sub.add_argument('--out', type=Path, required=True)
    sub.add_argument('--thresholds', type=Path)
    sub = subparsers.add_parser('report')
    sub.add_argument('--input', type=Path, required=True)
    sub.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == 'compare':
            metrics = compare(args.reference, args.candidate, args.out)
            thresholds = json.loads(args.thresholds.read_text()) if args.thresholds else {}
            result = {'schema_version': 1, 'metrics': metrics, **gate(metrics, thresholds)}
            write(args.out / 'comparison.json', result)
            return int(result['status'] == 'fail')
        if args.command == 'report':
            report(json.loads(args.input.read_text()), args.out)
            return 0
        config = load(args.config, args.preset.split(',') if args.preset else None)
        if args.command == 'list':
            rows, datasets = inventory(config)
            write(args.out, {'schema_version': 1, 'datasets': datasets, 'cases': rows})
            return 0
        if args.renderers:
            names = set(args.renderers.split(','))
            if names - {r['name'] for r in config['renderers']}:
                raise ValueError('unknown renderer name')
            config['renderers'] = [r for r in config['renderers'] if r['name'] in names]
            config['pairs'] = [p for p in config['pairs'] if set(p.get('renderers') if isinstance(p, dict) else p) <= names]
        return run(config, args)
    except (ValueError, OSError, TypeError, KeyError, ImportError) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    raise SystemExit(main())
