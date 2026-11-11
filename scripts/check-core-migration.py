#!/usr/bin/env python3
"""Reject retired module names in maintained build and API sources."""
from pathlib import Path
import re
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
# Match migration names, not iterator APIs, Vulkan pNext, or vendored symbols.
RETIRED = re.compile(
    r'\b(?:LIGHTUSD|TYDRA)_NEXT_[A-Z_]+|--next(?:\b|-)|'
    r'\b(?:lightusd|tydra)_next\b|lightusd::next\b|'
    r'\blightusd_static\b|\blightusd::lightusd\b|'
    r'\bnamespace\s+(?:[A-Za-z_]\w*::)*next(?:_\w+)?\b|'
    r'\bnext_(?:field|path_expr|usda_(?:parse|write|conn)|usdcat|pcp_\w*)\b|'
    r'\bnext(?:Diagnostic|Native|Glue|Counts|NodeIndexMap)\b|'
    r'\bnext[-/](?:only|local|pcp)\b|'
    r'\bnext (?:product|module|backend|architecture|implementation|PCP|headers|pipeline|converter)\b|'
    r'\bNext(?:AssetStore|RenderStream|LayerDocument|StageSession|FlattenSession|USDZConverterNative|RenderSceneAdapter)\b|'
    r'\bfunction\(\s*(?:add_test|set_tests_properties)\s*\)', re.IGNORECASE)
ROOTS = {'src', 'web', 'examples', 'tools', 'python', 'scripts', 'tests', 'cmake', '.github',
         'benchmark', 'benchmarks', 'aousd', 'sandbox', 'blender', 'android', 'ios', 'container', 'subprojects'}
SKIP = ('src/external/', 'src/nonstd/', 'src/lz4/', 'examples/common/',
        'web/js/src/lightusd/lightusd')
SUFFIXES = {'.cc', '.hh', '.c', '.h', '.py', '.js', '.mjs', '.cmake', '.sh', '.yml', '.yaml', '.json', '.cpp', '.hpp', '.txt', '.html', '.svg', '.mk', '.bat', '.ps1', '.toml', '.cfg', '.in', '.build', '.options'}
ROOT_CONFIGS = {'CMakeLists.txt', 'Makefile', 'meson.build', 'meson.options', 'setup.py',
                'pyproject.toml', 'MANIFEST.in', 'setup.cfg', 'CMakePresets.json'}
# Source archives have no Git metadata. Prune generated/vendor trees there
# instead of making the native test gate depend on a checkout.
if (ROOT / '.git').exists() and shutil.which('git'):
    paths = subprocess.check_output(
        ['git', 'ls-files', '--cached', '--others', '--exclude-standard'], cwd=ROOT,
        text=True).splitlines()
else:
    paths = sorted(ROOT_CONFIGS)
    for component in sorted(ROOTS):
        for directory, children, files in os.walk(ROOT / component):
            relative = Path(directory).relative_to(ROOT)
            children[:] = [child for child in children
                if child not in {'node_modules', 'dist', '__pycache__'}
                and not child.startswith(('.', 'build_', 'build-', 'cmake-build'))
                and child != 'build'
                and not (relative / child).as_posix().startswith(SKIP)]
            paths.extend((relative / name).as_posix() for name in files)
errors = []
for name in sorted(set(paths)):
    path = ROOT / name
    if (not path.is_file() or name.startswith(SKIP) or path == Path(__file__).resolve()
            or (name.split('/')[0] not in ROOTS and name not in ROOT_CONFIGS)
            or (path.suffix not in SUFFIXES and path.name not in {'CMakeLists.txt', 'Makefile'})):
        continue
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if RETIRED.search(line):
            errors.append(f'{name}:{number}: {line.strip()}')
if errors:
    print('\n'.join(errors), file=sys.stderr)
    sys.exit(1)
print('Core migration audit passed: no retired module build/API names')
