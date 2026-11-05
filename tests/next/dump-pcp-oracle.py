#!/usr/bin/env python3
"""Lossless PCP provenance from the pinned stock OpenUSD wheel.

Run with the interpreter/environment containing usd-core==26.11. This uses
Pcp in USD mode, like the supplemental corpus driver, and does not modify assets.
"""
import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import sys


def oracle_provenance(expected):
    import pxr
    from pxr import Usd
    distribution = importlib.metadata.distribution('usd-core')
    if distribution.version != expected:
        raise RuntimeError(f'expected usd-core=={expected}, got {distribution.version}')
    package_file = Path(distribution.locate_file('pxr/__init__.py')).resolve()
    if Path(pxr.__file__).resolve() != package_file:
        raise RuntimeError('pxr module does not belong to the pinned usd-core distribution')
    return {'package': 'usd-core', 'version': distribution.version,
            'usdVersion': list(Usd.GetVersion())}


def dump(filename, provenance, fallbacks):
    from pxr import Pcp, Sdf
    filename = Path(filename).resolve()
    try:
        root = Sdf.Layer.FindOrOpen(str(filename))
    except Exception as error:
        return {'schemaVersion': 2, 'status': 'parse-error', 'oracle': provenance,
                'Entry': filename.name, 'error': str(error)}
    if not root:
        return {'schemaVersion': 2, 'status': 'parse-error', 'oracle': provenance,
                'Entry': filename.name, 'error': 'root layer could not be read'}

    def spell(layer):
        path = Path(layer.realPath or layer.identifier).resolve()
        try:
            return Path(os.path.relpath(path, filename.parent)).as_posix()
        except ValueError:
            return path.as_posix()

    def stack(specs):
        return [{'layer': spell(spec.layer), 'path': str(spec.path)} for spec in specs]

    identifier = Pcp.LayerStackIdentifier(root)
    cache = Pcp.Cache(identifier, usd=True)
    cache.SetVariantFallbacks(fallbacks)
    layers, errors = cache.ComputeLayerStack(identifier)
    errors = list(errors)
    pseudo, root_errors = cache.ComputePrimIndex(Sdf.Path.absoluteRootPath)
    errors.extend(root_errors)
    children, prohibited = pseudo.ComputePrimChildNames()
    todo = [Sdf.Path.absoluteRootPath.AppendChild(child)
            for child in reversed(children) if child not in prohibited]
    composing = {}
    while todo:
        path = todo.pop()
        if str(path) in composing:
            raise RuntimeError('PCP returned a duplicate namespace path: ' + str(path))
        cache.RequestPayloads([path], [])
        index, local_errors = cache.ComputePrimIndex(path)
        errors.extend(local_errors)
        children, prohibited = index.ComputePrimChildNames()
        children = [child for child in children if child not in prohibited]
        names = index.ComputePrimPropertyNames()
        item = {'Prim Stack': stack(index.primStack)}
        if children:
            item['Child names'] = children
        if names:
            item['Property names'] = names
            properties = {}
            for name in sorted(names):
                prop_path = path.AppendProperty(name)
                prop_index, prop_errors = Pcp.BuildPrimPropertyIndex(prop_path, cache, index)
                errors.extend(prop_errors)
                properties[str(prop_path)] = stack(prop_index.propertyStack)
            item['Property stacks'] = properties
        composing[str(path)] = item
        todo.extend(path.AppendChild(child) for child in reversed(children))
    return {'schemaVersion': 2, 'status': 'ok', 'oracle': provenance,
            'Entry': filename.name, 'Composing': composing,
            'Layer Stack': [spell(layer) for layer in layers.layers],
            'Composition errors': [str(error) for error in errors]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('filename')
    parser.add_argument('--expected-version', default='26.11')
    parser.add_argument('--variant-fallback', action='append', default=[])
    args = parser.parse_args()
    fallbacks = {}
    for setting in args.variant_fallback:
        name, separator, choices = setting.partition('=')
        if not separator or not name or not choices:
            parser.error('variant fallback must be name=choice[,choice]')
        fallbacks[name] = choices.split(',')
    try:
        provenance = oracle_provenance(args.expected_version)
        result = dump(args.filename, provenance, fallbacks)
    except Exception as error:
        print('oracle failed: ' + str(error), file=sys.stderr)
        return 2
    json.dump(result, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write('\n')
    return 3 if result['status'] == 'parse-error' else 0


if __name__ == '__main__':
    sys.exit(main())
