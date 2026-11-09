#!/usr/bin/env python3
"""Export declarative USD shaderDefs.usda or MaterialX NodeDefs to checker JSON.

No shader code is executed. Renderer-specific discovery requires that renderer's
exporter to produce the same versioned JSON interface.
"""
import argparse
import importlib.util
import json
import re
from pathlib import Path
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location('schema_export',
        Path(__file__).with_name('generate-openusd-schema-manifest.py'))
usd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usd)


def without_comments(text):
    out, start, pos = [], 0, 0
    while pos < len(text):
        if text[pos] in '\"\'':
            pos = usd._skip_string(text, pos)
        elif text[pos] == '#':
            out.append(text[start:pos])
            end = text.find('\n', pos)
            pos = len(text) if end < 0 else end
            start = pos
        else:
            pos += 1
    return ''.join(out) + text[start:]


def export_usd(path):
    text = without_comments(path.read_text())
    result = []
    for m in re.finditer(r'^def\s+Shader\s+"([^"]+)"', text, re.M):
        pos = m.end()
        while text[pos].isspace(): pos += 1
        if text[pos] == '(':
            pos = usd._matching(text, pos, '(', ')') + 1
        while text[pos].isspace(): pos += 1
        end = usd._matching(text, pos, '{', '}')
        body = text[pos+1:end]
        ports = {'inputs': {}, 'outputs': {}}
        for p in usd._properties(body):
            for kind in ports:
                if p['name'].startswith(kind + ':'):
                    ports[kind][p['name'][len(kind)+1:]] = p['type']
        ident = re.search(r'info:id\s*=\s*"([^"]+)"', body)
        result.append(dict(identifier=ident.group(1) if ident else m.group(1),
                           sourceType='', source=path.name, **ports))
    return result


def export_mtlx(path):
    types = {'boolean': 'bool', 'integer': 'int', 'float': 'float',
             'color3': 'color3f', 'color4': 'color4f', 'vector2': 'float2',
             'vector3': 'float3', 'vector4': 'float4', 'matrix33': 'matrix3d',
             'matrix44': 'matrix4d', 'string': 'string', 'filename': 'asset',
             'surfaceshader': 'token', 'displacementshader': 'token',
             'volumeshader': 'token', 'material': 'token', 'lightshader': 'token'}
    text = path.read_text()
    if '<!DOCTYPE' in text or '<!ENTITY' in text:
        raise ValueError('DTD/entity declarations are not definition data')
    root = ET.fromstring(text)
    definitions = {n.attrib['name']: n for n in root.iter('nodedef')}
    def ports(name, active):
        if name in active or name not in definitions:
            raise ValueError('missing or cyclic NodeDef inheritance: ' + name)
        n = definitions[name]
        parent = n.get('inherit')
        p = ports(parent, active | {name}) if parent else {'inputs': {}, 'outputs': {}}
        for kind in ('input', 'output'):
            for item in n.findall(kind):
                ty = item.get('type')
                if ty not in types:
                    raise ValueError('unsupported MaterialX type: ' + str(ty))
                p[kind + 's'][item.attrib['name']] = types[ty]
        if not p['outputs'] and n.get('type') in types:
            p['outputs']['out'] = types[n.get('type')]
        return p
    return [dict(identifier=name, sourceType='mtlx', source=path.name,
                 **ports(name, set())) for name in sorted(definitions)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('inputs', type=Path, nargs='+')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    nodes = []
    for path in a.inputs:
        nodes.extend(export_mtlx(path) if path.suffix == '.mtlx' else export_usd(path))
    if not nodes: p.error('no shader definitions found')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps({'formatVersion': 1, 'shaders': nodes},
                                  indent=2, sort_keys=True) + '\n')


if __name__ == '__main__': main()
