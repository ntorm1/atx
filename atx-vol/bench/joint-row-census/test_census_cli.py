"""Small standalone CLI controls; never load the SPY fixture."""
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import census_audit

class CensusCliTests(unittest.TestCase):
    def setUp(self):
        self.binary=Path(os.environ['ATX_QUOTE_BENCH_BINARY']).resolve()
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.source=self.root/'quotes.csv';self.policy=self.root/'policy.json'
        fields=['row_id','chain_id','strike','is_call','spot','bid','ask','years','years_calendar',
                'rate','sdiv','ddiv_pv','input_status','exercise','underlier','root','expiry_days','snapshot_ns']
        with self.source.open('w',newline='') as f:
            writer=csv.writer(f);writer.writerow(fields)
            row=0
            for strike in range(75,126,2):
                sigma_t=.3*math.sqrt(.5);d1=math.log(100./strike)/sigma_t+.5*sigma_t
                call=100.*.5*math.erfc(-d1/math.sqrt(2.))-strike*.5*math.erfc(-(d1-sigma_t)/math.sqrt(2.))
                for side,price in ((1,call),(0,call+strike-100.)):
                    writer.writerow([row,17,strike,side,100,price-.001,price+.001,.5,.5,0,0,0,
                                     'ok','American','AAA','AAA',21000,1]);row+=1
        self.policy.write_text(json.dumps(dict(schema_version=1,admission='graded',
            graded=dict(cold_fit='joint_feasible_mid',certificate='box',certificate_box_half_ticks=0.),
            serve=dict(floor_price_only=True,metadata=True))))

    def invoke(self,name,extra=(),workers=1):
        env=dict(os.environ,ATX_VOL_FIT_WORKERS=str(workers))
        return subprocess.run([str(self.binary),'--input',str(self.source),'--output',str(self.root/(name+'.csv')),
            '--policy',str(self.policy),'--timing-detail','--cold-reset','--report',str(self.root/(name+'.report.json')),
            *extra],env=env,capture_output=True,text=True,timeout=60)

    def test_complete_capture_retains_fit_and_independent_integrity(self):
        capture=self.root/'capture.json'
        result=self.invoke('capture',['--joint-row-census',str(capture)])
        self.assertEqual(result.returncode,0,result.stderr)
        report=json.loads(result.stdout);observed=census_audit.strict_json(capture)
        summary=census_audit.validate_report(observed)
        self.assertTrue(summary['integrity_passed'])
        self.assertTrue(summary['final_full_row_scan_passed'])
        self.assertFalse(report['bench']['timing_qualifying'])
        self.assertEqual(observed['input_sha256'],hashlib.sha256(self.source.read_bytes()).hexdigest())
        self.assertEqual(observed['prediction_sha256'],hashlib.sha256((self.root/'capture.csv').read_bytes()).hexdigest())
        self.assertEqual(observed['executable_sha256'],hashlib.sha256(self.binary.read_bytes()).hexdigest())
        self.assertEqual(observed['counts']['nodes'],26)
        with (self.root/'capture.csv').open() as stream:
            self.assertEqual(len(list(csv.DictReader(stream))),52)
        self.assertGreaterEqual(report['bench']['joint_row_census']['export_and_destroy_s'],0.)

    def test_invalid_modes_and_fresh_path_requirements_refuse_before_outputs(self):
        for name,extra,workers in (
            ('workers',['--joint-row-census',str(self.root/'workers.json')],2),
            ('audit',['--joint-row-census',str(self.root/'audit.json'),'--acceptance-audit'],1),
            ('alias',['--joint-row-census',str(self.source)],1)):
            result=self.invoke(name,extra,workers)
            self.assertNotEqual(result.returncode,0)
            self.assertFalse((self.root/(name+'.csv')).exists())
        stale=self.root/'stale.json';stale.write_text('retained')
        result=self.invoke('stale',['--joint-row-census',str(stale)])
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(stale.read_text(),'retained')

if __name__=='__main__':unittest.main()
