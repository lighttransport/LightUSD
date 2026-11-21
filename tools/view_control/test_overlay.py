"""USD-backed editor tests. Run with the same Python/USD environment as usdview."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

try:
    from pxr import Sdf, Usd
except ImportError:
    Sdf = Usd = None

BRIDGE = Path(os.environ.get('VIEW_CONTROL_USDVIEW_SOURCE', Path(__file__).resolve().parents[3]/'OpenUSD/pxr/usdImaging/usdviewq'))


@unittest.skipUnless(Usd and (BRIDGE/'debugOverlay.py').exists(), 'requires USD and fork debugOverlay.py')
class OverlayTests(unittest.TestCase):
    def setUp(self):
        spec=importlib.util.spec_from_file_location('testedDebugOverlay',BRIDGE/'debugOverlay.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        self.temporary=tempfile.TemporaryDirectory()
        self.root=Path(self.temporary.name)
        self.source=self.root/'source.usda'
        self.source.write_text('''#usda 1.0
(defaultPrim = "World"\n upAxis = "Z"\n startTimeCode = 1\n endTimeCode = 20)
def Xform "World" {
 float value = 1
 float3 vector = (0,0,0)
 float3[] vectors = [(0,0,0)]
 matrix4d matrix = ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))
 def Scope "Target" {}
}
''')
        self.original=self.source.read_bytes()
        stage=Usd.Stage.Open(str(self.source))
        self.editor=module.DebugOverlay(stage,self.root/'debug')
        self.stage=Usd.Stage.Open(self.editor.root)

    def tearDown(self):
        self.assertEqual(self.source.read_bytes(),self.original)
        self.temporary.cleanup()

    def transaction(self, edits=None, action='edit'):
        result=self.editor.prepare(self.stage,action,edits)
        identifier=result['transaction'];self.editor.commit(identifier);self.editor.accept(identifier)

    def test_typed_edits_undo_redo(self):
        self.transaction([{'op':'set_attribute','path':'/World','property':'value','value':4.0},
                          {'op':'set_attribute','path':'/World','property':'vectors','value':[[1,2,3],[4,5,6]]},
                          {'op':'relationship','path':'/World','property':'debug:link','targets':['/World/Target']}])
        prim=self.stage.GetPrimAtPath('/World')
        self.assertEqual(prim.GetAttribute('value').Get(),4)
        self.assertEqual(len(prim.GetAttribute('vectors').Get()),2)
        self.assertEqual(prim.GetRelationship('debug:link').GetTargets(),[Sdf.Path('/World/Target')])
        self.transaction(action='undo');self.assertEqual(prim.GetAttribute('value').Get(),1)
        self.transaction(action='redo');self.assertEqual(prim.GetAttribute('value').Get(),4)
        self.assertEqual(self.stage.GetMetadata('upAxis'),'Z')
        self.assertEqual(self.stage.GetStartTimeCode(),1)
        self.assertEqual(self.stage.GetEndTimeCode(),20)

    def test_failed_batch_is_atomic(self):
        before=self.editor.overlay.ExportToString()
        with self.assertRaises(Exception):
            self.editor.prepare(self.stage,'edit',[{'op':'set_attribute','path':'/World','property':'value','value':9},
                                                   {'op':'set_attribute','path':'/Missing','property':'value','value':7}])
        self.assertEqual(self.editor.overlay.ExportToString(),before)
        self.assertIsNone(self.editor.pending)
        self.assertFalse(self.editor.status()['can_undo'])

    def test_committed_transaction_rolls_back_without_history(self):
        result=self.editor.prepare(self.stage,'edit',[{'op':'set_attribute','path':'/World','property':'value','value':9}])
        self.editor.commit(result['transaction'])
        self.assertEqual(self.stage.GetPrimAtPath('/World').GetAttribute('value').Get(),9)
        self.editor.rollback(result['transaction'])
        self.assertEqual(self.stage.GetPrimAtPath('/World').GetAttribute('value').Get(),1)
        self.assertFalse(self.editor.status()['can_undo'])
        self.assertFalse(self.editor.status()['can_redo'])

    def test_wrong_type_and_nonfinite_leave_no_pending(self):
        for value in (True, 'x', float('nan')):
            with self.assertRaises(Exception):
                self.editor.prepare(self.stage,'edit',[{'op':'set_attribute','path':'/World','property':'value','value':value}])
            self.assertIsNone(self.editor.pending)

    def test_new_edit_discards_redo(self):
        self.transaction([{'op':'set_attribute','path':'/World','property':'value','value':2}])
        self.transaction(action='undo')
        self.transaction([{'op':'set_attribute','path':'/World','property':'value','value':3}])
        self.assertFalse(self.editor.status()['can_redo'])
        self.assertEqual(self.stage.GetPrimAtPath('/World').GetAttribute('value').Get(),3)

    def test_matrix_and_clear_opinion(self):
        self.transaction([{'op':'set_attribute','path':'/World','property':'matrix',
                           'value':[2,0,0,0,0,2,0,0,0,0,2,0,0,0,0,1]}])
        self.assertEqual(self.stage.GetPrimAtPath('/World').GetAttribute('matrix').Get()[0][0],2)
        self.transaction([{'op':'clear_property','path':'/World','property':'matrix'}])
        self.assertEqual(self.stage.GetPrimAtPath('/World').GetAttribute('matrix').Get()[0][0],1)


    def test_variant_and_property_connections(self):
        fixture=self.root/'variant-source.usda'
        fixture.write_bytes(self.original)
        stage=Usd.Stage.Open(str(fixture))
        variants=stage.GetPrimAtPath('/World').GetVariantSets().AddVariantSet('choice')
        variants.AddVariant('A');variants.AddVariant('B');variants.SetVariantSelection('A')
        stage.GetRootLayer().Save()
        editor=type(self.editor)(stage,self.root/'variant-debug')
        target=Usd.Stage.Open(editor.root)
        edit=[{'op':'variant','path':'/World','set':'choice','value':'B'},
              {'op':'connections','path':'/World','property':'value','targets':['/World.vector']}]
        prepared=editor.prepare(target,'edit',edit)
        editor.commit(prepared['transaction']);editor.accept(prepared['transaction'])
        self.assertEqual(target.GetPrimAtPath('/World').GetVariantSet('choice').GetVariantSelection(),'B')
        self.assertEqual(target.GetPrimAtPath('/World').GetAttribute('value').GetConnections(),[Sdf.Path('/World.vector')])
        exported=editor.export()
        reopened=Usd.Stage.Open(exported['exported_wrapper'])
        self.assertEqual(reopened.GetPrimAtPath('/World').GetVariantSet('choice').GetVariantSelection(),'B')

    def test_partial_publish_failure_restores_previous_layer(self):
        prepared=self.editor.prepare(self.stage,'edit',[{'op':'set_attribute','path':'/World','property':'value','value':7}])
        original=self.editor._publish
        calls=[]
        def failing(text):
            original(text);calls.append(text)
            if len(calls)==1: raise RuntimeError('injected publication failure')
        self.editor._publish=failing
        with self.assertRaisesRegex(RuntimeError,'injected publication failure'):
            self.editor.commit(prepared['transaction'])
        self.assertEqual(self.stage.GetPrimAtPath('/World').GetAttribute('value').Get(),1)
        self.editor.rollback(prepared['transaction'])
        self.assertFalse(self.editor.status()['can_undo'])


    def test_unsaved_native_opinions_are_not_discarded(self):
        source=Usd.Stage.Open(str(self.source))
        with Usd.EditContext(source,source.GetSessionLayer()):
            source.GetPrimAtPath('/World').GetAttribute('value').Set(12)
        with self.assertRaisesRegex(ValueError,'unsaved native layer opinions'):
            type(self.editor)(source,self.root/'unsaved')
        self.assertEqual(source.GetPrimAtPath('/World').GetAttribute('value').Get(),12)


@unittest.skipUnless(Usd, 'requires USD Python')
class BuiltinLauncherTests(unittest.TestCase):
    def test_installed_launcher_remote_option_names(self):
        import argparse
        try:
            from pxr import Usdviewq
        except ImportError:
            self.skipTest('installed USD has no usdview/Qt package')
        parser=argparse.ArgumentParser()
        Usdviewq.Launcher().RegisterPositionals(parser)
        Usdviewq.Launcher().RegisterOptions(parser)
        if '--remote-control' not in parser._option_string_actions:
            self.skipTest('installed older USD has no built-in bridge')
        ordinary=parser.parse_args(['scene.usda'])
        self.assertFalse(ordinary.remote_control)
        options=parser.parse_args(['--remote-control','--remote-control-port','1234',
                                  '--remote-token-file','token','--remote-info-file','endpoint.json','scene.usda'])
        self.assertTrue(options.remote_control)
        self.assertEqual(options.remote_control_port,1234)
        self.assertTrue(options.remote_token_file.endswith('token'))
        self.assertTrue(options.remote_info_file.endswith('endpoint.json'))


if __name__=='__main__': unittest.main()
