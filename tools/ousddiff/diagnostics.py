"""Stable failure codes and bounded log excerpts for regression triage."""
import re
from pathlib import Path

SUCCESS_STATUSES = {'pass', 'support_or_empty', 'empty_for_purposes', 'xfail'}
LOG_PATTERNS = {
    'no_renderable_geometry': r'no renderable (?:Mesh triangles|geometry)',
    'materialx_nodedef_missing': r'could not find a nodedef|unable to find.*nodedef',
    'materialx_interface_mismatch': r"node interface error|input .+doesn't match declaration",
    'graphics_context_unavailable': r'could not connect to display|could not (?:load|initialize).*Qt platform|failed to create.*(?:GL|OpenGL).*context|cannot open display',
    'device_unavailable': r'no (?:suitable|usable).*(?:GPU|device)|failed to (?:create|initialize).*device|no ROCm device',
    'unresolved_dependency': r'unresolved dependencies|could not open asset|unresolved.*asset',
    'unsupported_feature': r'unsupported (?:prim|geometry|shader|schema)',
}


def analyze_log(text):
    findings = []
    for code, pattern in LOG_PATTERNS.items():
        matches = []
        occurrences = 0
        for line in text.splitlines():
            if re.search(pattern, line, re.I):
                occurrences += 1
                excerpt = line.strip()[:500]
                if excerpt not in matches and len(matches) < 3:
                    matches.append(excerpt)
        if occurrences:
            findings.append({'code': code, 'occurrences': occurrences, 'examples': matches})
    return findings


def read_log(path, limit=256 * 1024):
    if not path:
        return ''
    try:
        with Path(path).open('rb') as stream:
            stream.seek(0, 2)
            stream.seek(max(0, stream.tell() - limit))
            return stream.read().decode(errors='replace')
    except OSError:
        return ''


def annotate_execution(row):
    text = row.get('error', '') + '\n' + row.get('reason', '')
    for key in ('stderr', 'stdout'):
        text += '\n' + read_log(row.get(key))
    row['diagnostics'] = analyze_log(text)
    if row['status'] != 'ok' and not row['diagnostics']:
        row['diagnostics'] = [{'code': row['status'], 'occurrences': 1, 'examples': [text.strip()[-500:]]}]
    return row


def hardware_error(renderer, stats, identity=None):
    """Return a specific mismatch without erasing unrelated execution failures."""
    if renderer.get('require_hardware'):
        if stats.get('software_device'):
            return 'software_device', 'software GPU device rejected'
        if stats.get('fallback'):
            return 'backend_fallback', 'renderer backend fallback rejected'
        if renderer.get('kind') == 'lusdrender' and not stats.get('device'):
            return 'device_identity_missing', 'hardware identity missing from renderer output'
    if renderer.get('device_pattern'):
        # Storm obtains its identity from the context capability check; native
        # renderers identify their actual device in each render's own output.
        device = identity if identity is not None else stats.get('device')
        if device is not None or renderer.get('kind') != 'usdrecord':
            if not re.search(renderer['device_pattern'], device or '', re.I):
                return 'device_mismatch', 'device does not match ' + renderer['device_pattern']
    if renderer.get('backend_pattern') and not re.search(renderer['backend_pattern'], stats.get('backend', ''), re.I):
        return 'backend_mismatch', 'backend does not match ' + renderer['backend_pattern']
    return None


def failure_codes(case):
    codes = {d['code'] for d in analyze_log(case.get('prepared', {}).get('error', ''))}
    for name, stage in case.get('stages', {}).items():
        if stage.get('status') != 'ok':
            codes.update(d['code'] for d in stage.get('diagnostics', []))
            codes.add(name + '_failed')
    for render in case.get('renders', {}).values():
        if render.get('status') != 'ok' and render.get('required', True):
            codes.update(d['code'] for d in render.get('diagnostics', []))
            for attempt in render.get('attempts', []):
                if attempt['status'] != 'ok':
                    codes.update(d['code'] for d in attempt.get('diagnostics', []))
            if not codes:
                codes.add(render['status'])
    for pair in case.get('comparisons', []):
        if pair.get('status') == 'fail':
            codes.add('visual_threshold_failed')
        elif pair.get('status') == 'invalid_comparison':
            codes.add('invalid_comparison')
    if case.get('historical', {}).get('status') not in (None, 'pass'):
        codes.add('historical_comparison_failed')
    if case.get('status') not in SUCCESS_STATUSES and not codes:
        codes.add(case.get('status', 'unknown'))
    return sorted(codes)
