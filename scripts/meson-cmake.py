#!/usr/bin/env python3
"""Build canonical CMake targets for Meson without translating CMake semantics."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def within(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def run(command):
    subprocess.run(command, check=True, stdout=sys.stderr)


def split_fragment(text):
    return [part.strip('"') for part in shlex.split(text, posix=os.name != 'nt')]


def configure(args):
    build = Path(args.build).resolve()
    query = build / '.cmake/api/v1/query'
    query.mkdir(parents=True, exist_ok=True)
    for name in ['codemodel-v2', 'cmakeFiles-v1']:
        (query / name).touch()
    command = ['cmake', '-S', str(Path(args.source).resolve() / 'subprojects/lightusd-cmake'),
               '-B', str(build), '-G', 'Ninja'] + args.defines
    command += ['-DCMAKE_C_COMPILER=' + args.c_compiler,
                '-DCMAKE_CXX_COMPILER=' + args.cpp_compiler]
    sanitizers = [value for value in args.sanitize if value != 'none']
    sanitize_flag = ['-fsanitize=' + ','.join(sanitizers)] if sanitizers else []
    for language, flags in [('C', args.c_args), ('CXX', args.cpp_args)]:
        values = flags.splitlines() + sanitize_flag
        joined = subprocess.list2cmdline(values) if os.name == 'nt' else ' '.join(shlex.quote(value) for value in values)
        command.append('-DCMAKE_' + language + '_FLAGS=' + joined)
    if sanitize_flag:
        command += ['-DCMAKE_EXE_LINKER_FLAGS=' + sanitize_flag[0],
                    '-DCMAKE_SHARED_LINKER_FLAGS=' + sanitize_flag[0]]
    run(command)
    reply = build / '.cmake/api/v1/reply'
    index = json.loads(max(reply.glob('index-*.json'), key=lambda p: p.stat().st_mtime_ns).read_text())
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text())
    targets = {}
    artifacts = {}
    for entry in model['configurations'][0]['targets']:
        target = json.loads((reply / entry['jsonFile']).read_text())
        targets[target['name']] = target
        for artifact in target.get('artifacts', []):
            path = str((build / artifact['path']).resolve())
            artifacts[os.path.normcase(path)] = path
    root = Path(args.source).resolve()
    manifest = {'build': str(build), 'artifacts': {}, 'products': {}, 'inputs': []}
    for product, target_name in [('core', 'lightusd_core'), ('tydra', 'tydra'),
                                 ('legacy', 'lightusd_legacy_static'), ('shared', 'lightusd_c_shared'), ('legacy_shared', 'lightusd_legacy'),
                                 ('mcp', 'lightusd_mcp')]:
        probe = targets.get('lightusd_meson_' + product + '_probe')
        if not probe:
            continue
        data = {'target': target_name, 'outputs': [], 'link_args': [], 'includes': [], 'defines': []}
        for group in probe.get('compileGroups', []):
            for item in group.get('includes', []):
                include = Path(item['path']).resolve()
                data['includes'].append(str(include.relative_to(root))
                    if within(include, root) and not within(include, build.parent)
                    else str(include))
            data['defines'] += ['-D' + item['define'] for item in group.get('defines', [])]
        for fragment in probe.get('link', {}).get('commandFragments', []):
            if fragment['role'] not in {'libraries', 'libraryPath', 'frameworkPath'}:
                continue
            for token in split_fragment(fragment['fragment']):
                path = os.path.normcase(str((build / token).resolve()))
                if path in artifacts:
                    original = artifacts[path]
                    output = Path(original).name
                    if output in manifest['artifacts'] and manifest['artifacts'][output] != original:
                        raise RuntimeError('Colliding CMake archive names: ' + output)
                    manifest['artifacts'][output] = original
                    data['outputs'].append(output)
                else:
                    data['link_args'].append(token)
        if not data['outputs']:
            raise RuntimeError('No link artifacts found for ' + target_name)
        manifest['products'][product] = data
    cmake_files = json.loads((reply / index['reply']['cmakeFiles-v1']['jsonFile']).read_text())
    root = Path(args.source).resolve()
    for item in cmake_files['inputs']:
        path = Path(item['path'])
        if not path.is_absolute():
            path = Path(cmake_files['paths']['source']) / path
        path = path.resolve()
        if within(path, root) and not within(path, build) and path.is_file():
            manifest['inputs'].append(str(path))
    Path(args.manifest).write_text(json.dumps(manifest, indent=2) + '\n')


def build(args):
    manifest = json.loads(Path(args.manifest).read_text())
    targets = [item['target'] for item in manifest['products'].values()]
    command = ['cmake', '--build', manifest['build'], '--target'] + targets
    if args.jobs:
        command += ['--parallel', str(args.jobs)]
    run(command)
    destination = Path(args.output)
    destination.mkdir(parents=True, exist_ok=True)
    for name, original in manifest['artifacts'].items():
        source, output = Path(original), destination / name
        if output.exists() and source.stat().st_size == output.stat().st_size and source.stat().st_mtime_ns == output.stat().st_mtime_ns:
            continue
        temporary = output.with_name(output.name + '.tmp')
        shutil.copy2(source, temporary)
        temporary.replace(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    setup = sub.add_parser('configure')
    setup.add_argument('--source', required=True)
    setup.add_argument('--build', required=True)
    setup.add_argument('--manifest', required=True)
    setup.add_argument('--c-compiler', required=True)
    setup.add_argument('--cpp-compiler', required=True)
    setup.add_argument('--c-args', default='')
    setup.add_argument('--cpp-args', default='')
    setup.add_argument('--sanitize', nargs='*', default=[])
    setup.add_argument('defines', nargs='*')
    compile_parser = sub.add_parser('build')
    compile_parser.add_argument('--manifest', required=True)
    compile_parser.add_argument('--output', required=True)
    compile_parser.add_argument('--jobs', type=int, default=8)
    query = sub.add_parser('query')
    query.add_argument('--manifest', required=True)
    query.add_argument('--product')
    query.add_argument('field')
    args, extra = parser.parse_known_args()
    if args.action == 'configure':
        args.defines += extra
        configure(args)
    elif args.action == 'build':
        build(args)
    else:
        manifest = json.loads(Path(args.manifest).read_text())
        data = manifest['products'][args.product] if args.product else manifest
        value = data[args.field]
        print('\n'.join(value))


if __name__ == '__main__':
    main()
