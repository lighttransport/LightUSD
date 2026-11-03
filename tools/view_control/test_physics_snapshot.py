from pathlib import Path
import tempfile
import unittest
from physics_snapshot import snapshot
try:
    import lightusd
except ImportError:
    lightusd=None
try:
    from pxr import Usd
except ImportError:
    Usd=None


class SnapshotIntegrationTests(unittest.TestCase):
    def check_resolution_states(self,engine):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'states.usda'
            source.write_text('''#usda 1.0
 def Xform "Body" (prepend apiSchemas = ["PhysicsMassAPI", "PhysicsRigidBodyAPI"]) {
  float physics:mass = None
  float physics:declared
  float physics:blocked = None
  float physics:sample.timeSamples = {1: 2, 3: 4}
 }
''')
            result=snapshot(source,engine,[None,1,2,3]);body=result['prims']['/Body']
            attributes=body['attributes']
            self.assertEqual(attributes['physics:mass']['authored_default_state'],'blocked')
            self.assertEqual(attributes['physics:declared']['authored_default_state'],'absent')
            self.assertEqual(attributes['physics:blocked']['authored_default_state'],'blocked')
            self.assertEqual(attributes['physics:sample']['values']['None'],None)
            self.assertEqual(attributes['physics:sample']['values']['2'],3)
            self.assertTrue(body['effective_attributes']['physics:rigidBodyEnabled']['values']['None'])
            self.assertEqual(body['effective_attributes']['physics:density']['values']['None'],0)

    @unittest.skipIf(Usd is None,'requires SDK Python')
    def test_sdk_resolution_states(self):self.check_resolution_states('openusd')

    @unittest.skipIf(lightusd is None,'requires current LightUSD Python')
    def test_native_resolution_states(self):self.check_resolution_states('lightusd')

    def check_fixture(self,engine):
        source=Path(__file__).resolve().parents[2]/'tests/usda/physics-load-compare-root.usda'
        result=snapshot(source,engine,[None,0,.5,1]);body=result['prims']['/World/Body']
        self.assertEqual(body['attributes']['physics:mass']['values']['0.5'],4)
        self.assertEqual(body['attributes']['physics:velocity']['values']['0.5'],[1,0,0])
        self.assertEqual(result['prims']['/World/Joint']['relationships']['physics:body1'],['/World/Body'])
        self.assertIn('drive:angular:physics:stiffness',result['prims']['/World/Joint']['attributes'])
        self.assertEqual(result['metadata']['metersPerUnit'],.01)
        collider=result['prims']['/World/Body/Collider']
        self.assertEqual(collider['world_transform']['0.5'][3][1],1)
        self.assertEqual(collider['collision_geometry']['size']['values']['None'],1)

    @unittest.skipIf(Usd is None,'requires SDK Python')
    def test_sdk_composition_and_samples(self):self.check_fixture('openusd')

    @unittest.skipIf(lightusd is None,'requires current LightUSD Python')
    def test_native_composition_and_samples(self):self.check_fixture('lightusd')

    @unittest.skipIf(Usd is None,'requires SDK Python')
    def test_unknown_extension_schema_tokens_are_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            file=Path(tmp)/'extension.usda'
            file.write_text('#usda 1.0\ndef MjcActuator "A" (prepend apiSchemas = ["UnknownPhysicsAPI"]) {\n double[] mjc:gear = [1, 0, 0]\n}\n')
            result=snapshot(file,'openusd',[None,0])
            self.assertEqual(result['prims']['/A']['schemas'],['UnknownPhysicsAPI'])
            self.assertEqual(result['prims']['/A']['attributes']['mjc:gear']['values']['0'],[1.,0.,0.])

if __name__=='__main__':unittest.main()
