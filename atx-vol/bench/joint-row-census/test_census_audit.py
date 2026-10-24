import copy
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest

import census_audit as audit

FIXTURE = Path(__file__).with_name('fixtures') / 'two-slices.json'

def bits(x):
    return struct.pack('>d', x).hex()

class CensusReaderTests(unittest.TestCase):
    def fixture(self):
        return json.loads(FIXTURE.read_bytes())

    def test_fused_rounding_and_signed_zero(self):
        self.assertEqual(audit.fma(1.+2.**-27, 1.-2.**-27, -1.), -2.**-54)
        self.assertEqual(bits(audit.fma(-0., 1., -0.)), bits(-0.))
        self.assertEqual(bits(audit.fma(-1., 1., 1.)), bits(0.))
        self.assertEqual(bits(audit.fma(-2.**-1074, .25, 0.)), bits(-0.))
        for x in (math.inf, math.nan):
            with self.assertRaises(ValueError): audit.fma(x, 1., 0.)

    def test_complete_actual_native_fixture(self):
        raw = self.fixture()
        out = audit.validate_report(raw)
        self.assertTrue(out['integrity_passed'])
        self.assertFalse(out['timing_qualifying'])
        self.assertEqual(out['counts'], raw['counts'])
        self.assertEqual(out['bounds_retained'], 2*raw['counts']['variables'])
        self.assertEqual(out['graph_full']['matrix_nonzeros'], raw['profile']['matrix_nonzeros'])

    def test_fixed_raw_terminal_union_grid_and_feasible_fixtures(self):
        for name in ('fixed','raw','union','feasible'):
            r=json.loads(FIXTURE.with_name(name+'.json').read_bytes())
            with self.subTest(name=name):
                out=audit.validate_report(r)
                self.assertTrue(out['final_full_row_scan_passed'])
                if name=='union': self.assertGreater(out['row_kinds']['nondividend_lower'],0)
                if name=='fixed': self.assertLess(r['counts']['variables'],r['counts']['coordinates'])
                if name=='feasible': self.assertEqual(r['sweeps'],0)

    def test_actual_publication_clamp_and_node_mapping_cannot_be_replaced(self):
        for mutate in (lambda r:r['variables'][0].update(published=bits(42.)),
                       lambda r:r['nodes'][0].update(published=[bits(42.),bits(2.)]),
                       lambda r:r['slices'][0].update(terminal_discount=bits(.5)),
                       lambda r:r['profile'].update(matrix_nonzeros=1)):
            r=self.fixture();mutate(r)
            with self.assertRaises(ValueError):audit.validate_report(r)

    def test_coefficient_mutant_with_recomputed_slack_still_refuses(self):
        r=self.fixture();row=r['rows'][0]
        row['coefficients'][0]=bits(.9)
        decoded=dict(indices=row['indices'][:row['count']],
                     coefficients=[audit.number(v) for v in row['coefficients'][:row['count']]],
                     rhs=audit.number(row['rhs']))
        row['initial_slack']=bits(audit.slack(decoded,[audit.number(v['initial']) for v in r['variables']]))
        with self.assertRaises(ValueError):audit.validate_report(r)

    def test_strict_json_and_nonfinite_fields_refuse(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'bad.json'
            for text in ('{"a":1,"a":2}','{"a":NaN}','{"a":1e400}'):
                path.write_text(text)
                with self.assertRaises(ValueError):audit.strict_json(path)
        for encoded in ('7ff0000000000000','7ff8000000000000','+0','0'*17):
            r=self.fixture();r['nodes'][0]['strike']=encoded
            with self.assertRaises(ValueError):audit.validate_report(r)

    def test_arithmetic_artifact_and_executable_join_fail_closed(self):
        import hashlib
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);artifacts={}
            for name in ('executable','object','source','disassembly','cache'):
                data=(name+' synthetic\n').encode();(root/name).write_bytes(data)
                artifacts[name]=dict(path=name,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
            proof=dict(schema='atx-joint-row-arithmetic-evidence-v1',
                row_arithmetic='ordered_fma_binary64_nearest_v1',artifacts=artifacts,
                row_value_observation='Synthetic test only, not native codegen evidence')
            path=root/'proof.json';path.write_text(json.dumps(proof))
            r=self.fixture();r.update(executable_sha256=artifacts['executable']['sha256'],
                                     build_info=dict(joint_row_census=dict(enabled=True)))
            self.assertEqual(len(audit.bind_artifacts(r,path)),64)
            r['executable_sha256']='0'*64
            with self.assertRaises(ValueError):audit.bind_artifacts(r,path)
            r['executable_sha256']=artifacts['executable']['sha256']
            (root/'object').write_bytes(b'changed')
            with self.assertRaises(ValueError):audit.bind_artifacts(r,path)

    def test_changed_row_or_removed_constraint_refuses(self):
        for field, value in (('rhs', bits(42.)), ('initial_slack', bits(42.))):
            r = self.fixture(); r['rows'][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): audit.validate_report(r)
        r = self.fixture(); r['rows'].pop(0); r['counts']['rows'] -= 1
        r['diagnostics']['constraints'] -= 1; r['profile'] = None
        with self.assertRaises(ValueError): audit.validate_report(r)

    def test_initial_map_raw_and_capture_phase_refuse(self):
        edits = [lambda r: r['variables'][0].update(initial=bits(42.)),
                 lambda r: r['nodes'][0].update(raw=[bits(1.)]),
                 lambda r: r['coordinates'][0].update(variable=1),
                 lambda r: r.update(valid=False), lambda r: r.update(published_events=0),
                 lambda r: r['counts'].update(rows=True)]
        for edit in edits:
            r = self.fixture(); edit(r)
            with self.assertRaises(ValueError): audit.validate_report(r)

    def test_graph_keeps_zero_coefficient_index_edges(self):
        rows = [{'indices': [0, 1], 'coefficients': [1., 0.]}]
        g = audit.graph(rows, 3)
        self.assertEqual(g['matrix_nonzeros'], 4)
        self.assertEqual(g['component_sizes'], [2, 1])
        self.assertEqual(g['touched_variables'], 2)

if __name__ == '__main__': unittest.main()
