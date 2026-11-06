import json
import math
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from validation import sample_times,load_config,apply_capture_options
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

    def test_reference_camera_light_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'job.json'
            base={'version':1,'scenes':[{'path':'scene.usda'}]}
            path.write_text(json.dumps(dict(base,reference_camera_light=True)))
            self.assertTrue(load_config(path)['reference_camera_light'])
            for bad in ({'reference_camera_light':1},{'reference_camera_light':'yes'}):
                path.write_text(json.dumps(dict(base,**bad)))
                with self.assertRaises(ValueError):load_config(path)
            path.write_text(json.dumps(dict(base,reference_camera_light=True,
                beauty_lighting={'key':1.0})))
            with self.assertRaises(ValueError):load_config(path)

    def test_capture_options_survive_a_scene_state(self):
        # Job-level capture options must survive the per-scene state import;
        # otherwise reference_camera_light/exposure silently stop reaching the
        # candidate for jobs whose scenes carry a saved camera.
        state={'camera':{},'time':0}
        apply_capture_options(state, {'reference_camera_light':True,
                                      'exposure':2.0,
                                      'display_transform':'srgb'})
        self.assertIs(state['reference_camera_light'],True)
        self.assertEqual(state['exposure'],2.0)
        self.assertEqual(state['display_transform'],'srgb')
        untouched={}
        apply_capture_options(untouched, {})
        self.assertEqual(untouched,{})

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


class MtlxFilePrefixTests(unittest.TestCase):
    """The doubled-fileprefix repair must detect, repair and be idempotent."""

    MTLX = '''<materialx version="1.38" fileprefix="./textures/">
  <input name="file" type="filename" value="./textures/a.jpg" />
  <input name="file" type="filename" value="./textures/b.jpg" />
</materialx>
'''

    def test_detects_repairs_and_is_idempotent(self):
        import importlib.util
        from pathlib import Path
        import tempfile
        here = Path(__file__).resolve().parent
        spec = importlib.util.spec_from_file_location(
            'fix_mtlx', here / 'fix_mtlx_fileprefix.py')
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'tex').mkdir()
            (root / 'm.mtlx').write_text(self.MTLX)
            self.assertEqual(mod.main([str(root), '--check']), 1)
            self.assertEqual(mod.main([str(root)]), 0)
            doubled = root / 'textures' / 'textures'
            self.assertTrue(doubled.is_symlink())
            self.assertEqual(doubled.resolve(), (root / 'textures').resolve())
            self.assertEqual(mod.main([str(root), '--check']), 0)

    def test_non_doubled_prefix_is_left_alone(self):
        import importlib.util
        from pathlib import Path
        here = Path(__file__).resolve().parent
        spec = importlib.util.spec_from_file_location(
            'fix_mtlx', here / 'fix_mtlx_fileprefix.py')
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.assertEqual(mod.doubled_prefixes(
            '<materialx fileprefix="./tex/">\n'
            '<input name="file" value="a.jpg"/></materialx>'), set())


class CandidateSamplesTests(unittest.TestCase):
    """A job may request an explicit candidate supersample budget."""

    def _load(self, value):
        import json, tempfile
        from pathlib import Path
        import importlib.util
        here = Path(__file__).resolve().parent
        spec = importlib.util.spec_from_file_location(
            'validation_mod', here / 'validation.py')
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'job.json'
            job = {'version': 1, 'assets_root': '.', 'scenes': [{'path': 'a.usda'}]}
            if value is not None:
                job['candidate_samples'] = value
            p.write_text(json.dumps(job))
            return mod.load_config(p)

    def test_default_is_one(self):
        self.assertEqual(self._load(None).get('candidate_samples', 1), 1)

    def test_accepts_valid_budget(self):
        self.assertEqual(self._load(4)['candidate_samples'], 4)

    def test_rejects_out_of_range_and_non_int(self):
        for bad in (0, 65, True, 2.5, '4'):
            with self.assertRaises(ValueError):
                self._load(bad)


class DisplayTransformTests(unittest.TestCase):
    """A job may opt into usdrecord's plain sRGB display transform."""

    def _load(self, value):
        import json, tempfile
        from pathlib import Path
        import importlib.util
        here = Path(__file__).resolve().parent
        spec = importlib.util.spec_from_file_location(
            'validation_mod', here / 'validation.py')
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'job.json'
            job = {'version': 1, 'assets_root': '.', 'scenes': [{'path': 'a.usda'}]}
            if value is not None:
                job['display_transform'] = value
            p.write_text(json.dumps(job))
            return mod.load_config(p)

    def test_default_is_filmic(self):
        self.assertEqual(self._load(None).get('display_transform', 'filmic'), 'filmic')

    def test_accepts_srgb(self):
        self.assertEqual(self._load('srgb')['display_transform'], 'srgb')

    def test_rejects_unknown(self):
        for bad in ('aces', 'linear', 1, True):
            with self.assertRaises(ValueError):
                self._load(bad)


class ExposureTests(unittest.TestCase):
    """A job may override the beauty exposure (exp2 scale)."""

    def _load(self, value):
        import json, tempfile
        from pathlib import Path
        import importlib.util
        here = Path(__file__).resolve().parent
        spec = importlib.util.spec_from_file_location(
            'validation_mod', here / 'validation.py')
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'job.json'
            job = {'version': 1, 'assets_root': '.', 'scenes': [{'path': 'a.usda'}]}
            if value is not None:
                job['exposure'] = value
            p.write_text(json.dumps(job))
            return mod.load_config(p)

    def test_default_is_one(self):
        self.assertEqual(self._load(None).get('exposure', 1.0), 1.0)

    def test_accepts_range(self):
        for v in (-8, -0.5, 0, 1.0, 2.25, 8):
            self.assertEqual(self._load(v)['exposure'], v)

    def test_rejects_out_of_range_and_non_numeric(self):
        for bad in (9, -9, '1', True, None if False else [1]):
            with self.assertRaises(ValueError):
                self._load(bad)
