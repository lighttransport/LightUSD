"""Configuration, dataset inventory and reproducible sample selection."""
import copy
import fnmatch
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
USD_EXTENSIONS = {'.usd', '.usda', '.usdc', '.usdz'}


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, allow_nan=False).encode()).hexdigest()


def expand(value, base):
    value = os.path.expanduser(os.path.expandvars(value.replace('{repo}', str(REPO))))
    if '$' in value:
        raise ValueError('unresolved environment variable in ' + value)
    path = Path(value)
    return str((base / path).resolve()) if not path.is_absolute() else str(path)


def executable(value, base):
    value = os.path.expanduser(os.path.expandvars(value.replace('{repo}', str(REPO))))
    if '/' in value:
        return expand(value, base)
    return shutil.which(value) or value


def load(path, presets=None):
    path = Path(path).resolve()
    config = json.loads(path.read_text())
    if not isinstance(config, dict):
        raise ValueError('configuration must be a JSON object')
    for key in ('datasets', 'cases', 'renderers', 'pairs'):
        if key in config and not isinstance(config[key], list):
            raise ValueError(key + ' must be an array')
    for key in ('openusd', 'render', 'resources', 'thresholds', 'historical_thresholds', 'performance_thresholds', 'expected_failures'):
        if key in config and not isinstance(config[key], dict):
            raise ValueError(key + ' must be an object')
    if config.get('schema_version', 1) != 1:
        raise ValueError('unsupported configuration schema_version')
    base = path.parent
    config.setdefault('datasets', [])
    config.setdefault('cases', [])
    config.setdefault('render', {})
    config.setdefault('resources', {})
    config.setdefault('pairs', [])
    config.setdefault('renderers', [])
    if presets and presets != ['all']:
        available = {d['name'] for d in config['datasets']}
        if set(presets) - available:
            raise ValueError('unknown preset: ' + ','.join(sorted(set(presets) - available)))
        config['datasets'] = [d for d in config['datasets'] if d['name'] in presets]
    for dataset in config['datasets']:
        dataset['root'] = expand(dataset['root'], base)
    for case in config['cases']:
        case['path'] = expand(case['path'], base)
    openusd = config.get('openusd', {})
    for name in ('dist', 'python'):
        if name in openusd:
            openusd[name] = expand(openusd[name], base)
    if 'converter' in config:
        config['converter'] = executable(config['converter'], base)
    for renderer in config['renderers']:
        if not isinstance(renderer, dict) or not isinstance(renderer.get('env', {}), dict) or not isinstance(renderer.get('args', []), list) or not all(isinstance(v, str) for v in renderer.get('args', [])):
            raise ValueError('renderer requires an object, string-array args, and object env')
        if not re.fullmatch(r'[A-Za-z0-9_-]+', renderer['name']):
            raise ValueError('renderer names must be safe directory identifiers')
        if renderer.get('kind') not in (None, 'usdrecord', 'lusdrender', 'custom'):
            raise ValueError('unknown renderer kind')
        if renderer.get('kind', 'custom') == 'custom':
            if not isinstance(renderer.get('argv'), list) or not renderer['argv'] or not all(isinstance(v, str) for v in renderer['argv']):
                raise ValueError('custom argv must be a nonempty string array')
            renderer['argv'][0] = executable(renderer['argv'][0], base)
        else:
            renderer['executable'] = executable(renderer['executable'], base)
        renderer['env'] = {k: os.path.expanduser(os.path.expandvars(v.replace('{repo}', str(REPO)))) for k, v in renderer.get('env', {}).items()}
    names = [r['name'] for r in config['renderers']]
    if len(set(names)) != len(names):
        raise ValueError('duplicate renderer names')
    for pair in config['pairs']:
        members = pair.get('renderers') if isinstance(pair, dict) else pair
        if not isinstance(members, list) or len(members) != 2 or any(n not in names for n in members):
            raise ValueError('comparison pairs require two configured renderers')
    for key, default in [('timeout_s', 1800), ('max_rss_mib', 30720), ('reserve_mib', 4096)]:
        value = config['resources'].setdefault(key, config.get(key, default))
        if not isinstance(value, (int, float)) or not math.isfinite(value) or value < (0 if key == 'reserve_mib' else 0.001):
            raise ValueError('invalid resource setting ' + key)
    for key in ('repeat', 'width', 'height'):
        value = config['render'].get(key, 1)
        if not isinstance(value, int) or isinstance(value, bool) or value < 1:
            raise ValueError('render.' + key + ' must be a positive integer')
    if not isinstance(config['render'].get('warmups', 0), int) or config['render'].get('warmups', 0) < 0:
        raise ValueError('render.warmups must be a nonnegative integer')
    from images import gate
    gate({}, config.get('thresholds', {}))
    gate({}, config.get('historical_thresholds', {}))
    for item in config['pairs']:
        if isinstance(item, dict):
            gate({}, item.get('thresholds', {}))
    for item in config['cases']:
        gate({}, item.get('thresholds', {}))
    purposes = config['render'].get('purposes', ['default', 'render'])
    if not isinstance(purposes, list) or 'default' not in purposes or set(purposes) - {'default', 'render', 'proxy', 'guide'}:
        raise ValueError('purposes must include default and use known USD purposes')
    if config['render'].get('complexity', 'low') not in ('low', 'medium', 'high', 'veryhigh'):
        raise ValueError('unknown render complexity')
    tolerance = config.get('pixel_tolerance', 8)
    if not isinstance(tolerance, int) or not 0 <= tolerance <= 255:
        raise ValueError('pixel_tolerance must be an integer from 0 to 255')
    for policy in [config['render']] + [item.get('render', {}) for item in config['cases']]:
        if not isinstance(policy, dict):
            raise ValueError('case render settings must be an object')
        for key in ('width', 'height', 'repeat'):
            value = policy.get(key, 1)
            if not isinstance(value, int) or isinstance(value, bool) or value < 1:
                raise ValueError('render.' + key + ' must be a positive integer')
        frame = policy.get('frame', 'default')
        if frame != 'default' and (not isinstance(frame, (int, float)) or not math.isfinite(frame)):
            raise ValueError('render.frame must be a finite number or default')
        if policy.get('camera') and not str(policy['camera']).startswith('/'):
            raise ValueError('render.camera must be an absolute prim path')
    for metric, policy in config.get('performance_thresholds', {}).items():
        if metric not in ('wall_s', 'peak_rss_bytes', 'peak_tree_rss_bytes') or not isinstance(policy, dict):
            raise ValueError('unknown performance threshold or invalid policy')
        for key, value in policy.items():
            if key not in ('relative', 'absolute') or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError('performance limits require finite nonnegative relative/absolute values')
    config['_config_path'] = str(path)
    return config


def metadata(path):
    stat = Path(path).stat()
    return {'path': str(path), 'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns}


def classify(relative):
    path = relative.lower()
    if any(word in path for word in ('material', '/lighting/', '/cameras/', '.manifest.', '.topology.')):
        return 'support_candidate'
    if any(word in path for word in ('/mesh/', '/payload/', 'generated_proxies/', 'generated_splined/', '.geo.', '.gdt.', 'model.usd')):
        return 'geometry'
    return 'composition'


def inventory(config):
    rows, datasets = [], []
    for spec in config.get('datasets', []):
        root = Path(spec['root'])
        info = {'name': spec['name'], 'root': str(root), 'available': root.is_dir(), 'excluded': {}, 'revision': None, 'required': spec.get('required', True)}
        if root.is_dir():
            import subprocess
            try:
                info['revision'] = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True, text=True, timeout=5).stdout.strip() or None
            except (OSError, subprocess.TimeoutExpired):
                pass
        datasets.append(info)
        if not root.is_dir():
            continue
        full_roots = set(spec.get('full_scene_roots', []))
        globs = spec.get('include', ['**/*'])
        xml_candidates = []
        for directory, dirs, files in os.walk(root):
            dirs[:] = sorted(d for d in dirs if not d.startswith('.') and d not in ('__pycache__', 'node_modules'))
            for name in sorted(files):
                path = Path(directory) / name
                relative = path.relative_to(root).as_posix()
                reason = None
                if name.startswith('._'):
                    reason = 'appledouble'
                elif any(fnmatch.fnmatch(relative, pattern) for pattern in spec.get('exclude', [])):
                    reason = 'exclude_filter'
                elif not any(fnmatch.fnmatch(relative, p) or (p.startswith('**/') and fnmatch.fnmatch(relative, p[3:])) for p in globs):
                    reason = 'include_filter'
                elif path.suffix.lower() not in USD_EXTENSIONS | ({'.xml'} if spec.get('kind') == 'menagerie' else set()):
                    continue
                if reason:
                    info['excluded'][reason] = info['excluded'].get(reason, 0) + 1
                    continue
                row = {'id': spec['name'] + '/' + relative, 'dataset': spec['name'], 'path': str(path),
                       'relative_path': relative, 'root': str(root), 'fingerprint': metadata(path),
                       'bytes': path.stat().st_size, 'group': classify(relative), 'full_scene': relative in full_roots}
                if path.suffix.lower() == '.xml':
                    xml_candidates.append(row)
                    continue
                rows.append(row)
                if row['full_scene'] and spec.get('full_variants'):
                    full = copy.deepcopy(row)
                    full.update(id=row['id'] + '#full-geometry', variant_rules=spec['full_variants'], fidelity='full')
                    full['render'] = {'purposes': ['default', 'render']}
                    rows.append(full)
        for row in xml_candidates:
            try:
                xml = ET.parse(row['path']).getroot()
                if xml.tag != 'mujoco' or (xml.find('worldbody') is None and xml.find('include') is None):
                    info['excluded']['xml_fragment'] = info['excluded'].get('xml_fragment', 0) + 1
                    continue
                # Inclusion fragments containing only actuators are not models.
                if Path(row['path']).name == 'filtered_cartesian_actuators.xml':
                    info['excluded']['xml_fragment'] = info['excluded'].get('xml_fragment', 0) + 1
                    continue
                row.update(input_kind='mjcf', group='composition' if xml.find('include') is not None else 'geometry')
            except ET.ParseError as exc:
                row['inventory_error'] = str(exc)
            rows.append(row)
    for explicit in config.get('cases', []):
        row = copy.deepcopy(explicit)
        row.setdefault('dataset', 'explicit')
        row.setdefault('group', 'composition')
        if Path(row['path']).exists():
            row.update(fingerprint=metadata(row['path']), bytes=Path(row['path']).stat().st_size)
        else:
            row['inventory_error'] = 'missing input'
        rows.append(row)
    if len({r['id'] for r in rows}) != len(rows):
        raise ValueError('duplicate case IDs')
    return sorted(rows, key=lambda r: r['id']), datasets


def sample(rows, percent, seed):
    if not math.isfinite(percent) or not 1 <= percent <= 10:
        raise ValueError('probe percentage must be between 1 and 10')
    groups = {}
    for row in rows:
        key = (row.get('dataset', 'explicit'), row.get('group', 'composition'), int(math.log10(max(row.get('bytes', 1), 1))))
        groups.setdefault(key, []).append(row)
    selected = []
    for group in groups.values():
        ranked = sorted(group, key=lambda r: hashlib.sha256((str(seed) + ':' + r['id']).encode()).digest())
        selected.extend(ranked[:max(1, math.ceil(len(group) * percent / 100))])
    return sorted(selected, key=lambda r: r['id'])
