import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from headless import HeadlessSession,dispatch,import_state,normalize


class HeadlessTests(unittest.TestCase):
    def test_successful_exit_does_not_hide_partial_reference_shaders(self):
        from headless import reference_render_findings
        with tempfile.TemporaryDirectory() as root:
            log=Path(root)/'render.log'
            log.write_text('Warning: deprecated asset version\n')
            self.assertEqual(reference_render_findings(log),[])
            log.write_text(('Warning: Failed to link shader: missing function\n'*20)+'GL error: invalid value\n')
            found=reference_render_findings(log)
            self.assertEqual(len(found),8)
            self.assertIn('Failed to link shader',found[0])

    def test_reference_resource_loss_is_incomplete(self):
        from headless import reference_render_findings
        with tempfile.TemporaryDirectory() as root:
            log=Path(root)/'render.log'
            messages=[
                'Warning: Hydra Storm supports up to 16 lights, truncating the 24 found lights to this max.',
                'Warning: Loading volume.vdb failed! No fallback value available, using default!',
                "Error: [PluginLoad] Unknown field data type 'vdb' for file 'volume.vdb'",
                'Recording aborted due to the following failure at time code 0.0:',
                'GL error: out of memory',
            ]
            for message in messages:
                log.write_text(message+'\n')
                self.assertEqual(reference_render_findings(log),[message])
                self.assertEqual(reference_render_findings(log,lighting_sensitive=False),
                                 [] if 'truncating' in message else [message])

    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.scene=self.root/'source.usda';self.scene.write_text('#usda 1.0\ndef Cube "World" {}\n')
        self.args=SimpleNamespace(scene=self.scene,out=self.root/'session',state=None,
            usdrecord=self.root/'sdk/bin/usdrecord',usd_python=Path(sys.executable),
            lusdrender=self.root/'lusdrender',usd_env=[],backend='cpu',timeout=.05)

    def tearDown(self): self.temp.cleanup()

    def camera(self):
        return {'transform':[1,0,0,0,0,1,0,0,0,0,1,0,0,0,5,1],
                'projection':'perspective','fov_y':60,'aspect':4/3,'ortho_height':3,
                'shift':[.2,-.1],'near':.1,'far':100,'distance':5,'exposure':0}

    def test_import_gui_capture(self):
        state=import_state({'states':{'usdview':{'camera':self.camera(),'size':[640,480],
                           'timeline':{'time':5},'selection':['/World'],
                           'display':{'skeleton_control':True,'skeleton_policy':'native'}}}},self.scene)
        self.assertEqual(state['time'],5);self.assertEqual(state['camera']['shift'],[.2,-.1])
        self.assertEqual(state['selection'],['/World'])
        self.assertEqual(import_state({'state':state},self.scene),state)
        with self.assertRaises(ValueError): import_state({'unrelated':1},self.scene)

    def test_reusable_mcp_output_root_creates_fresh_private_sessions(self):
        self.args.session_subdir=True
        first=HeadlessSession(self.args);second=HeadlessSession(self.args)
        self.assertNotEqual(first.out,second.out)
        self.assertEqual(first.out.parent,self.args.out.resolve())
        self.assertTrue((first.out/'state.json').exists())
        self.assertTrue((second.out/'state.json').exists())

    def test_reusable_mcp_output_root_through_symlink(self):
        target=self.root/'real-output';target.mkdir()
        alias=self.root/'output-alias'
        try:alias.symlink_to(target,target_is_directory=True)
        except OSError as error:self.skipTest('directory symlinks unavailable: '+str(error))
        self.args.out=alias;self.args.session_subdir=True
        session=HeadlessSession(self.args)
        self.assertEqual(session.out.parent,target.resolve())
        self.assertTrue((session.out/'state.json').is_file())

    def test_reject_unrepresentable_state(self):
        base=normalize({'scene':str(self.scene),'camera':self.camera()})
        for field,value in [('mode','wireframe'),('hud',True),('background','magenta'),('guide',1),('unsupported',False)]:
            state=copy.deepcopy(base);state['display'][field]=value
            with self.assertRaises(ValueError): normalize(state)
        for key,value in [('aspect',2),('near',-1),('far',.001),('exposure',1),('fov_y',float('nan'))]:
            state=copy.deepcopy(base);state['camera'][key]=value
            with self.assertRaises(ValueError): normalize(state)
        state=copy.deepcopy(base);state['camera']['transform'][0]=2
        with self.assertRaises(ValueError): normalize(state)
        for size in ([16,False],[0,10],[4097,16],[1e9,10]):
            with self.assertRaises(ValueError): normalize(dict(base,size=size))

    def test_configure_and_overlay_failures_are_atomic(self):
        session=HeadlessSession(self.args);original=copy.deepcopy(session.status())
        with patch.object(session,'_prepare',side_effect=RuntimeError('stage validation failed')):
            with self.assertRaises(RuntimeError): session.configure({'selection':['/absent']})
            with self.assertRaises(RuntimeError): session.overlay('edit',[{'op':'bad'}])
        self.assertEqual(original,session.status())
        self.assertEqual(session.history,[[]]);self.assertEqual(session.position,0)
        with patch.object(session,'_prepare',return_value=({},{})):
            session.overlay('edit',[{'op':'set_attribute','path':'/World','property':'size','value':2}])
            session.overlay('undo');self.assertEqual(session.state['edits'],[])
            session.overlay('redo');self.assertEqual(session.state['edits'][0]['value'],2)
            session.overlay('clear');self.assertEqual(session.state['edits'],[])
        self.assertEqual(self.scene.read_text(),'#usda 1.0\ndef Cube "World" {}\n')

    def test_each_native_renderer_runs_even_when_the_other_fails(self):
        session=HeadlessSession(self.args);directory=self.root/'job';directory.mkdir()
        prepared={'camera':self.camera(),'time':0,'wrapper':str(self.scene),'camera_path':'/Camera','dependencies':[],'diagnostic_segments':0}
        with patch.object(session,'_prepare',return_value=(directory,prepared)), patch.object(session,'_dependencies'), patch.object(session,'_run',side_effect=[RuntimeError('reference failed'),RuntimeError('candidate failed')]) as run:
            with self.assertRaisesRegex(RuntimeError,'reference failed.*candidate failed'):session.render()
            self.assertEqual(run.call_count,2)
        failure=json.loads((directory/'failure.json').read_text())
        self.assertEqual(set(failure['process_errors']),{'usdrecord','lusdrender'})
        self.assertIsNone(session.latest)

    def test_timeout_cleans_owned_process(self):
        session=HeadlessSession(self.args);directory=self.root/'run';directory.mkdir()
        with self.assertRaises(subprocess.TimeoutExpired):
            session._run([sys.executable,'-c','import time; time.sleep(30)'],directory,'test')
        self.assertIsNone(session.active)
        session.close()
        with self.assertRaises(RuntimeError): session._run([sys.executable,'-V'],directory,'closed')

    def test_dependency_change_rejects_pair(self):
        stat=self.scene.stat();dependencies={'dependencies':[{'path':str(self.scene),'size':stat.st_size,'mtime_ns':stat.st_mtime_ns}]}
        HeadlessSession._dependencies(dependencies)
        self.scene.write_text('# changed\n')
        with self.assertRaises(RuntimeError): HeadlessSession._dependencies(dependencies)

    def test_dependency_hash_rejects_same_size_preserved_mtime_edit(self):
        import os
        from headless import file_fingerprint
        self.scene.write_text('original')
        stat=self.scene.stat()
        prepared={'dependencies':[dict(path=str(self.scene),size=stat.st_size,
                    mtime_ns=stat.st_mtime_ns,sha256=file_fingerprint(self.scene)['sha256'])]}
        self.scene.write_text('modified')
        os.utime(self.scene,ns=(stat.st_atime_ns,stat.st_mtime_ns))
        with self.assertRaisesRegex(RuntimeError,'dependency changed'):
            HeadlessSession._dependencies(prepared)

    def test_toolchain_change_rejects_capture(self):
        self.args.lusdrender.write_text('renderer revision one')
        session=HeadlessSession(self.args)
        self.args.lusdrender.write_text('renderer revision two')
        directory=self.root/'job';directory.mkdir()
        with self.assertRaisesRegex(RuntimeError,'toolchain changed'):
            session._run([sys.executable,'-V'],directory,'changed')
        self.assertIsNone(session.active)

    def test_sdk_runtime_change_rejects_capture_even_with_preserved_mtime(self):
        import os
        self.args.usdrecord=self.root/'sdk/bin/usdrecord'
        self.args.usdrecord.parent.mkdir(parents=True)
        self.args.usdrecord.write_text('test executable')
        schema=self.root/'sdk/lib/usd/generatedSchema.usda'
        schema.parent.mkdir(parents=True)
        schema.write_text('fallback one')
        session=HeadlessSession(self.args)
        self.assertEqual(len(session.toolchain['sdk_runtime']['files']),1)
        stat=schema.stat()
        schema.write_text('fallback two')
        os.utime(schema,ns=(stat.st_atime_ns,stat.st_mtime_ns))
        directory=self.root/'job';directory.mkdir()
        with self.assertRaisesRegex(RuntimeError,'toolchain changed'):
            session._run([sys.executable,'-V'],directory,'changed-schema')
        self.assertIsNone(session.active)

    def test_stdio_schemas_and_failed_tool(self):
        session=HeadlessSession(self.args)
        tools=dispatch(session,{'jsonrpc':'2.0','id':1,'method':'tools/list'})['result']['tools']
        self.assertEqual({t['name'] for t in tools},{'status','configure','render','inspect_prim','overlay','image','stop'})
        failure=dispatch(session,{'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'configure','arguments':{'state':{'display':{'hud':True}}}}})
        self.assertIn('error',failure);self.assertEqual(session.revision,0)
        self.assertIn('error',dispatch(session,None))


if __name__=='__main__': unittest.main()


class BackendMappingTests(unittest.TestCase):
    def test_instanced_modes_are_explicit_renderer_flags(self):
        from headless import backend_arguments
        self.assertEqual(backend_arguments('vkr-streamed'),['-vkInstancedStream'])
        self.assertEqual(backend_arguments('vkr-instanced'),['-vkInstanced'])
        with self.assertRaises(ValueError):backend_arguments('unknown')


if __name__ == "__main__": unittest.main()


@unittest.skipUnless(sys.platform.startswith('linux'),'Linux process supervision')
class HeadlessResourceTests(unittest.TestCase):
    setUp=HeadlessTests.setUp
    tearDown=HeadlessTests.tearDown

    def test_sampled_rss_budget_terminates_child_and_records_reason(self):
        self.args.timeout=5
        self.args.process_limits={'max_rss_mib':8,'reserve_mib':0}
        session=HeadlessSession(self.args);directory=self.root/'job';directory.mkdir()
        with self.assertRaisesRegex(RuntimeError,'resource_limit'):
            session._run([sys.executable,'-c','import time; x=bytearray(32*1024*1024); time.sleep(10)'],directory,'memory')
        resource=json.loads((directory/'memory.resources.json').read_text())
        self.assertEqual(resource['status'],'resource_limit')
        self.assertIn('RSS',resource['reason'])
        self.assertIsNone(session.active)

    def test_counter_budget_fails_before_process_start(self):
        counter=self.root/'vram';counter.write_text(str(2*1024**2))
        self.args.process_limits={'gpu_vram_path':str(counter),'gpu_budget_mib':1,'reserve_mib':0}
        session=HeadlessSession(self.args);directory=self.root/'job';directory.mkdir()
        with self.assertRaisesRegex(RuntimeError,'resource_limit'):
            session._run([sys.executable,'-V'],directory,'gpu')
        resource=json.loads((directory/'gpu.resources.json').read_text())
        self.assertEqual(resource['reason'],'initial whole-device GPU memory budget')
        self.assertIsNone(resource['returncode'])
        self.assertIsNone(session.active)
