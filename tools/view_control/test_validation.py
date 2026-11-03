import json
import math
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from validation import sample_times,load_config
try:
    from aov import compare_data,read_pfm
except ModuleNotFoundError as error:
    if error.name != 'PIL': raise
    compare_data=read_pfm=None


class ValidationTests(unittest.TestCase):
    def test_full_storage_preserves_previous_report(self):
        from validation import atomic_text
        with tempfile.TemporaryDirectory() as tmp:
            target=Path(tmp)/'validation.json';target.write_text('{"previous":true}')
            original=Path.write_text
            def full(path,text):
                original(path,'partial')
                raise OSError(28,'No space left on device')
            with patch.object(Path,'write_text',full):
                with self.assertRaises(OSError):atomic_text(target,'{"next":true}')
            self.assertEqual(target.read_text(),'{"previous":true}')
            self.assertFalse(target.with_name(target.name+'.tmp').exists())
            atomic_text(target,'{"next":true}')
            self.assertEqual(json.loads(target.read_text()),{'next':True})

    def test_background_agreement_cannot_validate_surfaces(self):
        from validation import require_visible_surfaces
        for metrics in ({},{'reference_coverage_pixels':0,'candidate_coverage_pixels':0},
                        {'reference_coverage_pixels':4,'candidate_coverage_pixels':0}):
            frame={'aovs':{'depth':{'metrics':metrics},'color':{'verdict':'PASS'},
                          'roughness':{'verdict':'PASS','native_parameter':{'verdict':'PASS'}},
                          'metallic':{'verdict':'FAIL'}}}
            require_visible_surfaces(frame)
            self.assertEqual(frame['aovs']['color']['verdict'],'INCOMPLETE')
            self.assertEqual(frame['aovs']['roughness']['native_parameter']['verdict'],'INCOMPLETE')
            self.assertEqual(frame['aovs']['metallic']['verdict'],'FAIL')
        frame={'aovs':{'depth':{'metrics':{'reference_coverage_pixels':4,'candidate_coverage_pixels':3,'common_pixels':3}},
                       'baseColor':{'verdict':'PASS'}}}
        require_visible_surfaces(frame)
        self.assertEqual(frame['aovs']['baseColor']['verdict'],'PASS')

    def test_sampling_replay_and_subframes(self):
        self.assertEqual(sample_times({'mode':'uniform','count':3},1,50),[1,25.5,50])
        config={'mode':'random','count':4,'seed':21}
        values=sample_times(config,0,30)
        self.assertEqual(values,sample_times(config,0,30));self.assertEqual(values,sorted(values))
        self.assertTrue(all(0<=v<=30 for v in values))
        self.assertNotEqual(values,sample_times(dict(config,seed=22),0,30))
        self.assertEqual(sample_times({},7,7),[7])
        self.assertEqual(sample_times({'mode':'explicit','values':[3,.5]},0,1),[3,.5])
        for bad in ({'count':True},{'count':0},{'mode':'random','seed':False},{'mode':'explicit','values':[math.nan]},{'mode':'typo'},{'start':2,'end':1}):
            with self.assertRaises(ValueError):sample_times(bad,0,1)

    def test_population_mask_is_explicit_and_requires_isolation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'job.json'
            scene={'path':'scene.usda','isolate':['/Selected'],'reference_population_mask':True}
            path.write_text(json.dumps({'version':1,'scenes':[scene]}))
            self.assertTrue(load_config(path)['scenes'][0]['reference_population_mask'])
            for change in ({'isolate':[]},{'reference_population_mask':1}):
                path.write_text(json.dumps({'version':1,'scenes':[dict(scene,**change)]}))
                with self.assertRaises(ValueError):load_config(path)

    def test_reference_complexity_is_explicit_and_bounded(self):
        from headless import reference_complexity
        for value in ('low','medium','high','veryhigh'):
            self.assertEqual(reference_complexity(value),value)
        for value in ('ultra',1,None,[]):
            with self.assertRaises(ValueError):reference_complexity(value)
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'job.json'
            path.write_text(json.dumps({'version':1,'reference_complexity':'medium',
                'scenes':[{'path':'sphere.usda','reference_complexity':'high'}]}))
            self.assertEqual(load_config(path)['scenes'][0]['reference_complexity'],'high')

    def test_beauty_lighting_contract_is_explicit_and_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'job.json'
            base={'version':1,'scenes':[{'path':'scene.usda'}]}
            path.write_text(json.dumps(dict(base,beauty_lighting={'key':1.0,'fill':0.25,
                'dome':0.1,'key_rotate':[-45,0,0]})))
            self.assertEqual(load_config(path)['beauty_lighting']['key'],1.0)
            path.write_text(json.dumps(dict(base,beauty_lighting={'key':1.0},
                scenes=[{'path':'scene.usda','beauty_lighting':{'key':2.0,'fill':0.5}}])))
            self.assertEqual(load_config(path)['scenes'][0]['beauty_lighting']['fill'],0.5)
            for bad in ({'key':-1},{'fill':'x'},{'dome':True},{'force':1},
                        {'key_rotate':[1,2]},{'fill_rotate':'up'},{'typo':1}):
                path.write_text(json.dumps(dict(base,beauty_lighting=bad)))
                with self.assertRaises(ValueError):load_config(path)

    def test_config_rejects_unknown_and_unbounded_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'job.json';base={'version':1,'scenes':[{'path':'scene.usda'}]}
            for bad in ({'thresholds':{'typo':0}},{'size':[10000,10]},{'aovs':['not-an-aov']},{'backend':'invalid'},{'data_backend':'hip'},{'native_curve_segments':0},{'native_curve_segments':True},{'native_curve_segments':65},{'camera_policy':'invalid'},{'scenes':[{'path':'scene.usda','state':{'edits':[{}]},'aovs':['baseColor']}]}):
                path.write_text(json.dumps(dict(base,**bad)))
                with self.assertRaises(ValueError):load_config(path)

    @unittest.skipIf(compare_data is None,'requires controller Pillow dependency')
    def test_raw_depth_projection_and_normal_spaces(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);w=h=8;near,far=1,100;z=5
            camera={'near':near,'far':far,'fov_y':60,'aspect':1,'shift':[0,0],'projection':'perspective',
                    'transform':[0,0,-1,0,0,1,0,0,1,0,0,0,0,0,5,1]}
            gates={'coverage_iou_min':.99,'depth_relative_p95_max':1e-5,'normal_p95_degrees_max':.01}
            def write(name,values):
                path=root/name
                with path.open('wb') as f:
                    f.write(f'PF\n{w} {h}\n-1.0\n'.encode())
                    for y in range(h-1,-1,-1):
                        for x in range(w):f.write(struct.pack('<3f',*values(x,y)))
                return path
            d=(far-near*far/z)/(far-near)
            ref=write('ref.pfm',lambda x,y:(d,d,d))
            def distance(x,y):
                t=math.tan(math.radians(30));v=z*math.sqrt(1+((2*(x+.5)/w-1)*t)**2+((1-2*(y+.5)/h)*t)**2)
                return (v,v,v)
            cand=write('cand.pfm',distance)
            self.assertTrue(compare_data(ref,cand,camera,'depth',root/'depth',gates)['passed'])
            ref=write('ref.pfm',lambda x,y:(-.6,0,.8));cand=write('cand.pfm',lambda x,y:(.8,0,.6))
            self.assertTrue(compare_data(ref,cand,camera,'worldNormal',root/'normal',gates)['passed'])
            wrong=write('wrong.pfm',lambda x,y:(0,1,0))
            self.assertFalse(compare_data(ref,wrong,camera,'worldNormal',root/'wrong',gates)['passed'])
            empty=write('empty.pfm',lambda x,y:(0,0,0))
            self.assertFalse(compare_data(empty,empty,camera,'worldNormal',root/'empty',gates)['passed'])
            # Depth independently exposes a missing SDK normal, even when the
            # remaining normal samples agree. This must never become a pass.
            depth_ref=write('depth-ref.pfm',lambda x,y:(d,d,d))
            depth_cand=write('depth-cand.pfm',distance)
            ref_gap=write('ref-gap.pfm',lambda x,y:(0,0,0)if(x,y)==(3,3)else(-.6,0,.8))
            good=write('good-normal.pfm',lambda x,y:(.8,0,.6))
            result=compare_data(ref_gap,good,camera,'worldNormal',root/'gap',gates,depth_ref,depth_cand)
            self.assertTrue(result['comparison_passed']);self.assertFalse(result['passed'])
            self.assertEqual(result['reference_invalid_normal_pixels'],1)
            self.assertEqual(result['candidate_invalid_normal_pixels'],0)
            wrong.write_bytes(b'PF\n8 8\n-1\n')
            with self.assertRaises(ValueError):read_pfm(wrong)

if __name__=='__main__':unittest.main()


class NativeLoadLimitsTests(unittest.TestCase):
    def test_explicit_counts_and_memory_are_validated_and_replayed(self):
        from headless import native_load_arguments
        self.assertEqual(native_load_arguments({}),[])
        self.assertEqual(native_load_arguments({'max_memory_gib':30,'max_array_elements':67108864}),
                         ['-maxMem','30','-maxArrayElements','67108864'])
        for limits in (None,{'typo':1},{'max_memory_gib':True},{'max_memory_gib':float('nan')},
                       {'max_memory_gib':0},{'max_memory_gib':1025},{'max_array_elements':True},
                       {'max_array_elements':-1},{'max_array_elements':2**32},
                       {'max_array_elements':1.5}):
            with self.assertRaises(ValueError):native_load_arguments(limits)
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'job.json'
            p.write_text(json.dumps({'version':1,'native_load_limits':{'max_memory_gib':30},
                                    'scenes':[{'path':'scene.usda','native_load_limits':{'max_array_elements':67108864}}]}))
            config=load_config(p)
            self.assertEqual(config['scenes'][0]['native_load_limits']['max_array_elements'],67108864)
