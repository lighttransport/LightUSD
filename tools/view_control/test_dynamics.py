import copy
import math
from pathlib import Path
import tempfile
import json
import unittest
from dynamics import assess,load_config,make_mjcf

class DynamicsTests(unittest.TestCase):
    def case(self,kind='freefall'):
        return dict(id='test',kind=kind,dt=.001,steps=2,sample_every=1,position_atol=.001,velocity_atol=.001)

    def trajectory(self):
        return [dict(time=t,position=[0,0,2-.5*9.81*t*t],linear_velocity=[0,0,-9.81*t],joint_q=[],joint_qd=[])for t in (0,.001,.002)]

    def test_analytic_gravity_and_missing_coverage(self):
        case=self.case();samples=self.trajectory()
        self.assertTrue(assess(case,samples)['passed'])
        for changed in (samples[:-1],None,[{}]*3):self.assertFalse(assess(case,changed)['passed'])
        changed=copy.deepcopy(samples);changed[1]['time']=math.nan
        self.assertFalse(assess(case,changed)['passed'])
        changed=copy.deepcopy(samples);changed[1]['position'][2]=math.inf
        self.assertFalse(assess(case,changed)['passed'])
        changed=copy.deepcopy(samples);changed[1]['position'][2]+=.1
        self.assertFalse(assess(case,changed)['passed'])

    def test_contact_requires_observed_impact_and_settling(self):
        case=self.case('contact');samples=self.trajectory()
        self.assertFalse(assess(case,samples)['passed'])
        for sample in samples:sample.update(position=[0,0,.1],linear_velocity=[0,0,0],contact_count=1)
        self.assertTrue(assess(case,samples)['passed'])
        for sample in samples:sample['contact_count']=0
        self.assertFalse(assess(case,samples)['passed'])
        self.assertFalse(assess(case,[{}]*3)['passed'])

    def test_explicit_contact_gap_preserves_omission(self):
        import xml.etree.ElementTree as ET
        case=self.case('contact')
        self.assertNotIn('gap',ET.fromstring(make_mjcf(case)).find('default/geom').attrib)
        case['contact_gap']=0
        self.assertEqual(ET.fromstring(make_mjcf(case)).find('default/geom').get('gap'),'0')

    def test_config_bounds_and_fixture(self):
        with tempfile.TemporaryDirectory()as root:
            path=Path(root)/'job.json';config=dict(version=1,cases=[self.case()])
            path.write_text(json.dumps(config));self.assertEqual(load_config(path),config)
            self.assertIn('gravity="0 0 -9.81"',make_mjcf(config['cases'][0]))
            for key,value in [('steps',True),('dt',math.nan),('id','../escape'),('kind','unknown'),('contact_gap',-.01),('contact_gap',math.inf)]:
                changed=copy.deepcopy(config);changed['cases'][0][key]=value;path.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):load_config(path)

    def test_explicit_contact_response_and_bounds(self):
        import xml.etree.ElementTree as ET
        case=self.case('contact')
        self.assertNotIn('solref',ET.fromstring(make_mjcf(case)).find('default/geom').attrib)
        case['contact_solref']=[.005,1]
        self.assertEqual(ET.fromstring(make_mjcf(case)).find('default/geom').get('solref'),'0.005 1')
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'job.json'
            path.write_text(json.dumps(dict(version=1,cases=[case])))
            self.assertEqual(load_config(path)['cases'][0]['contact_solref'],[.005,1])
            for value in (None,True,[.005],[True,1],[math.nan,1],[.001,1],[.005,0],[-100,-10]):
                bad=dict(case,contact_solref=value);path.write_text(json.dumps(dict(version=1,cases=[bad])))
                with self.assertRaises(ValueError):load_config(path)
            bad=dict(case,kind='freefall');path.write_text(json.dumps(dict(version=1,cases=[bad])))
            with self.assertRaises(ValueError):load_config(path)

if __name__=='__main__':unittest.main()
