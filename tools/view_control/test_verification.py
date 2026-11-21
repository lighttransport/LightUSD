import unittest
from verification import adjudicate,performance_eligibility

class VerificationTests(unittest.TestCase):
    def test_agreement_and_controls_are_not_proof(self):
        rows=[{'gate':'physics','status':'PASS','owner':'openusd'}]
        self.assertFalse(adjudicate(rows,['physics'])['passed'])
        rows[0].update(contract='analytic gravity',evidence=['fixture'],control=True)
        self.assertFalse(adjudicate(rows,['physics'])['passed'])
        rows[0]['control']=False
        self.assertTrue(adjudicate(rows,['physics'])['passed'])
        self.assertFalse(adjudicate(rows,['physics','dynamics'])['complete'])
        rows.append({'gate':'physics','status':'FAIL','owner':'unresolved'})
        self.assertFalse(adjudicate(rows,['physics'])['passed'])
    def test_known_limit_is_not_a_pass(self):
        for state in ['ERROR','UNSUPPORTED','INCOMPLETE']:
            result=adjudicate([{'gate':'physics','status':state}],['physics'])
            self.assertFalse(result['passed']);self.assertFalse(result['complete'])
    def test_performance_requires_equal_correct_work(self):
        row=dict(status='PASS',correctness_passed=True,work_fingerprint='geometry',capture_semantics='raw-v1',purposes=['default'],size=[640,480],time=0,quality='one-sample',wall_s=2,peak_rss_bytes=100)
        self.assertTrue(performance_eligibility(row,row)['comparable'])
        for changed in [dict(row,quality='simplified'),dict(row,correctness_passed=False),dict(row,contended=True),dict(row,wall_s=float('nan'))]:
            self.assertFalse(performance_eligibility(row,changed)['comparable'])



class CatalogueTests(unittest.TestCase):
    def test_missing_changed_and_control_evidence_cannot_pass(self):
        from verification import verify_catalogue
        from pathlib import Path
        import tempfile,hashlib
        catalogue={'schema':1,'cases':[{'id':'rig','required':['deformation']},{'id':'physics','required':['dynamics']}]}
        with tempfile.TemporaryDirectory()as root:
            path=Path(root)/'proof.json';path.write_text('analytic fixture result')
            artifact={'path':'proof.json','sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
            row=dict(case='rig',gate='deformation',status='PASS',owner='lightusd',contract='known transform',evidence=[artifact])
            bundle=dict(version=1,assertions=[row])
            result=verify_catalogue(catalogue,bundle,root)
            self.assertTrue(result['cases'][0]['passed']);self.assertFalse(result['passed'])
            self.assertEqual(result['cases'][1]['missing_gates'],['dynamics'])
            row['control']=True;self.assertFalse(verify_catalogue(catalogue,bundle,root)['cases'][0]['passed'])
            del row['control'];path.write_text('changed')
            self.assertFalse(verify_catalogue(catalogue,bundle,root)['cases'][0]['passed'])
            artifact['path']='../outside'
            self.assertFalse(verify_catalogue(catalogue,bundle,root)['cases'][0]['passed'])

if __name__=='__main__':unittest.main()
