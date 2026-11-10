#!/usr/bin/env python3
"""Core large-scene example payload budgets, streaming and cache release."""
import pathlib
import os
import subprocess
import sys
import tempfile

app=sys.argv[1]
def run(file,*options,success=True):
    result=subprocess.run([app,str(file),*options],capture_output=True,text=True)
    assert (result.returncode==0)==success,result.stdout+result.stderr
    if success: assert 'failed' not in (result.stdout+result.stderr).lower(),result.stdout+result.stderr
    return result.stdout
with tempfile.TemporaryDirectory(prefix='lightusd-large-example-') as temporary:
    folder=pathlib.Path(temporary)
    (folder/'payload.usda').write_text('#usda 1.0\ndef Xform "Asset" {\n def Scope "Geo" { int marker=1; }\n}\n')
    root=folder/'root.usda'
    root.write_text('''#usda 1.0
def Xform "World" {
 def Xform "A" (prepend payload=@payload.usda@</Asset>) {
  float3[] extentsHint=[(0,0,0),(2,2,2)]
 }
 def Xform "B" (prepend payload=@payload.usda@</Asset>) {
  float3[] extentsHint=[(0,0,0),(1,1,1)]
 }
}
''')
    for extra in ([],['--no-mmap-zero-copy'],['--no-dedup']):
        text=run(root,'--mode=none','--load-some=1','--unload',*extra)
        assert 'deferred payloads = 2' in text,text
        assert 'loaded 1/1, deferred now = 1' in text,text
        assert 'unloaded 1/1, deferred now = 2' in text,text
        text=run(root,'--mode=all',*extra)
        assert 'total prims       = 5' in text,text
        assert 'deferred payloads = 0' in text,text
        text=run(root,'--mode=budget','--budget-mb=0',*extra)
        assert 'deferred payloads = 2' in text,text
        text=run(root,'--mode=budget','--budget-mb=1',*extra)
        assert 'deferred payloads = 0' in text,text
        text=run(root,'--mode=budget','--budget-mb=1','--extent-budget=4','--load-some=1','--unload',*extra)
        assert 'deferred payloads = 1' in text,text
        assert 'loaded 1/1, deferred now = 0' in text,text
        assert 'unloaded 1/1, deferred now = 1' in text,text
    assert 'deferred payloads = 2' in run(os.path.relpath(root),'--mode=none')
    if len(sys.argv)>2:
        crate=folder/'root.usdc'
        result=subprocess.run([sys.argv[2],'--rewrite-layer',str(root),'-o',str(crate)],capture_output=True,text=True)
        assert result.returncode==0,result.stdout+result.stderr
        for flag in ([],['--no-mmap-zero-copy'],['--no-dedup']):
            text=run(crate,'--mode=none','--load-some=2','--unload',*flag)
            assert 'loaded 2/2, deferred now = 0' in text,text
            assert 'unloaded 2/2, deferred now = 2' in text,text
    for option in ['--budget-mb=-1','--budget-mb=no','--budget-mb=18446744073709551616','--load-some=-1','--load-some=2147483648','--extent-budget=nan']:
        run(root,option,success=False)
    run(folder/'missing.usda',success=False)
print('PASS: core large-scene deferred/all/byte/extent policies, streaming, cache release and invalid arguments')
