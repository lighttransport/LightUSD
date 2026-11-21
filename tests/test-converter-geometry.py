#!/usr/bin/env python3
"""Native mesh merging is available for both flat and packaged root formats."""
import pathlib
import subprocess
import sys
import tempfile

converter, cat, repository = sys.argv[1:]
fixture = pathlib.Path(repository) / 'tests/usda/geometry-optimize-nonuniform-normal-001.usda'
with tempfile.TemporaryDirectory(prefix='lightusd-geometry-') as temporary:
    directory = pathlib.Path(temporary)
    for output_format, root_format in [('usda', 'usda'), ('usdc', 'usdc'), ('usdz', 'usda'), ('usdz', 'usdc')]:
        output = directory / f'{root_format}.{output_format}'
        result = subprocess.run([converter, str(fixture), str(output), '--outputFormat', output_format,
                                 '--rootLayerFormat', root_format, '-optimizeGeometry', 'mergeMeshes'],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        normalized = directory / 'normalized.usda'
        subprocess.run([cat, '--rewrite-layer', str(output), '-o', str(normalized)], check=True, capture_output=True, text=True)
        text = normalized.read_text()
        assert text.count('def Mesh ') == 1, text
        assert 'Merged_0' in text and '0.447213' in text, text
    failed = subprocess.run([converter, str(fixture), str(directory / 'bad.usdz'),
                             '-noFlatten', '-optimizeGeometry', 'mergeMeshes'], capture_output=True, text=True)
    assert failed.returncode == 0 and 'ignoring -noFlatten' in failed.stderr, failed.stderr
print('PASS: native geometry merge in USDA/USDC/USDZ and forced-flatten diagnostic')
