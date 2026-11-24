#!/usr/bin/env python3
"""Consumer omissions must not turn an incomplete physics comparison green."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('mujoco_compare',Path(__file__).with_name('run-mujoco-usd-compare.py'))
compare=importlib.util.module_from_spec(spec);spec.loader.exec_module(compare)


def model():
    return dict(counts={key:0 for key in compare.COUNTS},
        option={key:0 for key,_,_ in compare.OPTION},stderr='',
        **{key:[] for key in compare.FIELDS})


class CoverageTests(unittest.TestCase):
    def run_models(self,reference,candidate):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'source.xml';source.write_text('<mujoco><worldbody/></mujoco>')
            output=root/'report.json'
            importer=SimpleNamespace(import_mjcf=lambda *a,**k:SimpleNamespace(save=lambda p:None))
            argv=['compare',str(source),'--mujoco',tmp,'--dump',str(root/'dump'),'--out',str(output)]
            with patch.object(sys,'argv',argv),patch.dict(sys.modules,{'lightusd':importer}),patch.object(compare,'_dump',side_effect=[reference,candidate]),contextlib.redirect_stdout(io.StringIO()):
                code=compare.main()
            return code,json.loads(output.read_text())

    def test_omitted_spherical_joint_is_incomplete(self):
        reference=model();candidate=model()
        reference['counts']['njnt']=1
        reference['joints']=[{'name':'hip','type':compare.BALL}]
        code,result=self.run_models(reference,candidate)
        self.assertEqual(code,2)
        self.assertEqual(result['totals']['pass'],0)
        self.assertEqual(result['results'][0]['status'],'incomplete')
        self.assertFalse(result['results'][0]['coverage_complete'])
        self.assertTrue(result['results'][0]['decoder_limits'])

    def test_missing_free_joint_mapping_is_incomplete(self):
        reference=model();candidate=model()
        reference['counts']['njnt']=candidate['counts']['njnt']=1
        reference['joints']=[{'name':'root','type':compare.FREE}]
        candidate['joints']=[{'name':'','type':compare.FREE}]
        code,result=self.run_models(reference,candidate)
        self.assertEqual(code,2)
        self.assertEqual(result['results'][0]['status'],'incomplete')

    def test_real_mismatch_returns_failure(self):
        reference=model();candidate=model();reference['counts']['nbody']=1
        code,result=self.run_models(reference,candidate)
        self.assertEqual(code,1)
        self.assertEqual(result['results'][0]['status'],'mismatch')

    def test_complete_checked_fields_can_pass(self):
        reference=model()
        reference['counts']['nbody']=1
        reference['bodies']=[dict(name='body',mass=1,ipos=[0,0,0],
                                  inertia=[1,1,1],gravcomp=[0])]
        code,result=self.run_models(reference,copy.deepcopy(reference))
        self.assertEqual(code,0)
        self.assertTrue(result['results'][0]['coverage_complete'])

    def test_empty_physics_is_incomplete(self):
        code,result=self.run_models(model(),model())
        self.assertEqual(code,2)
        self.assertFalse(result['results'][0]['coverage_complete'])


if __name__=='__main__':unittest.main()
