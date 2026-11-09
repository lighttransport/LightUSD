import copy
import unittest
from physics_compare import compare
from physics_snapshot import plain,physics_property,geometry_value


class PhysicsComparisonTests(unittest.TestCase):
    def snapshot(self):
        return {'times':[None,0,.5,1],'metadata':{'upAxis':'Z','metersPerUnit':1},'scope':'test',
            'prims':{'/Body':{'type':'Xform','schemas':['PhysicsRigidBodyAPI','MjcBodyAPI'],
                'attributes':{'physics:mass':{'type':'float','uniform':False,'connections':[],
                    'values':{'None':2,'0':2.,'0.5':3.,'1':4.}},
                    'mjc:gear':{'type':'double[]','values':{'None':[1.,0.,0.,0.,0.,0.]}}},
                'relationships':{'physics:body1':['/Other']}}}}

    def test_missing_properties_targets_units_types_and_samples_fail(self):
        source=self.snapshot();self.assertTrue(compare(source,copy.deepcopy(source))['passed'])
        cases=[]
        for name in ['type','schemas','attributes','relationships']:
            changed=copy.deepcopy(source);del changed['prims']['/Body'][name];cases.append(changed)
        changed=copy.deepcopy(source);changed['metadata']['metersPerUnit']=.01;cases.append(changed)
        changed=copy.deepcopy(source);changed['prims']['/Body']['relationships']['physics:body1']=['/Missing'];cases.append(changed)
        changed=copy.deepcopy(source);changed['prims']['/Body']['attributes']['physics:mass']['values']['0.5']=-3;cases.append(changed)
        changed=copy.deepcopy(source);changed['prims']['/Body']['attributes']['mjc:gear']['values']['None'][0]=2;cases.append(changed)
        for changed in cases:self.assertFalse(compare(source,changed)['passed'])

    def test_geometry_digests_detect_equal_count_changes(self):
        first=geometry_value('points',[(0.,-0.,1.),(2.,3.,4.)])
        equivalent=geometry_value('points',[(0.,0.,1.),(2.,3.,4.)])
        changed=geometry_value('points',[(0.,0.,1.),(2.,3.,5.)])
        self.assertEqual(first,equivalent);self.assertEqual(first['components'],6)
        self.assertNotEqual(first['sha256'],changed['sha256'])
        with self.assertRaises(ValueError):geometry_value('points',[(float('nan'),0.,0.)])

    def test_multiapply_and_material_properties_are_in_scope(self):
        for name in ('mjc:gear','drive:angular:physics:stiffness','limit:transX:physics:low','material:binding:physics','collection:colliders:includes'):
            self.assertTrue(physics_property(name))
        self.assertFalse(physics_property('points'))

    def test_limits_empty_coverage_and_tolerance(self):
        source=self.snapshot();changed=copy.deepcopy(source);changed['prims']={}
        self.assertFalse(compare(source,changed)['passed'])
        empty=copy.deepcopy(source);empty['prims']={};self.assertFalse(compare(empty,empty)['complete'])
        limited=compare(source,{'times':[],'metadata':{},'prims':{},'scope':'test'},max_issues=1)
        self.assertFalse(limited['passed']);self.assertFalse(limited['complete']);self.assertGreater(limited['omitted'],0)
        for value in [-1,float('nan'),True]:
            with self.assertRaises(ValueError):compare(source,source,rtol=value)
        self.assertEqual(plain(float('inf')),{'nonfinite':'inf'})

if __name__=='__main__':unittest.main()
