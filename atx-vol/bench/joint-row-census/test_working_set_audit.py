import unittest

import working_set_audit as audit


def fixture():
    slots = []
    for rows, added in ((20, 2), (22, 0)):
        slots.append(dict(full_fallback=False, selected_rows=rows, iterations=3,
            factorizations=2, directions=4, matrix_nonzeros=14, factor_nonzeros_peak=8,
            iterate_scan_rows=40, publication_scan_rows=40, violated_published_rows=added,
            newly_added_rows=added, min_iterate_slack=-float(added),
            min_published_slack=-float(added), outcome=1))
    first = dict(present=True, synthetic_control=False, injected_refusals=0,
        full_variables=8, full_rows=40, bound_rows=16, initial_scan_rows=40,
        initial_violated_rows=4, selection_scan_rows=40, full_scan_rows=240,
        workspace_bytes=3000, fallback_reason=0, solves=slots)
    return dict(schema='joint-working-set-work-v1', invocations=1,
        synthetic_invocations=0, injected_refusals=0,
        slot_detail_invocations_omitted=0, full_variables_sum=8, full_rows_sum=40,
        bound_rows_sum=16, initial_scan_rows=40, initial_violated_rows=4,
        selection_scan_rows=40, attempts=2, reduced_solves=2, full_solves=0,
        iterations=6, factorizations=4, directions=8, full_scan_rows=240,
        newly_added_rows=2, workspace_bytes_requested_sum=3000, allocation_refusals=0,
        capacity_refusals=0, first_invocation=first)


class WorkEvidence(unittest.TestCase):
    def test_complete_bounded_work(self):
        self.assertTrue(audit.verify_work(fixture()))

    def test_lost_work_or_unlabeled_omission_refuses(self):
        for key in ('iterations', 'factorizations', 'directions', 'full_scan_rows',
                    'newly_added_rows', 'full_rows_sum', 'attempts'):
            data = fixture();data[key] -= 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                audit.verify_work(data)
        data = fixture();data['invocations'] = 2
        with self.assertRaises(ValueError):audit.verify_work(data)

    def test_omitted_rows_cannot_disappear_and_scans_cannot_be_partial(self):
        for key, value in (('selected_rows', 20), ('publication_scan_rows', 39),
                           ('iterate_scan_rows', 0), ('newly_added_rows', 50)):
            data = fixture();data['first_invocation']['solves'][1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                audit.verify_work(data)

    def test_budget_types_nonfinite_and_synthetic_refuse(self):
        for key, value in (('iterations', 201), ('factorizations', True), ('outcome', 9),
                           ('min_published_slack', float('nan'))):
            data = fixture();data['first_invocation']['solves'][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):audit.verify_work(data)
        data = fixture();data['first_invocation']['synthetic_control'] = True
        data['synthetic_invocations'] = 1
        with self.assertRaises(ValueError):audit.verify_work(data)
        self.assertTrue(audit.verify_work(data, allow_synthetic=True))

    def test_later_calls_keep_truthful_totals_and_explicit_omission(self):
        data = fixture()
        for key in audit.TOTALS:data[key] *= 2
        data['slot_detail_invocations_omitted'] = 1
        self.assertTrue(audit.verify_work(data))
        data['full_scan_rows'] = 1
        with self.assertRaises(ValueError):audit.verify_work(data)

    def test_fallback_must_be_last_and_use_every_row(self):
        data = fixture();data['first_invocation']['solves'][0]['full_fallback'] = True
        with self.assertRaises(ValueError):audit.verify_work(data)
        data = fixture();data['first_invocation']['fallback_reason'] = 3
        with self.assertRaises(ValueError):audit.verify_work(data)
        data = fixture();data['first_invocation']['fallback_reason'] = 8
        with self.assertRaises(ValueError):audit.verify_work(data)

    def test_native_denominator_and_work_are_required(self):
        data = dict(graded_stats=dict(slices=[dict(timing=dict(joint_working_set=fixture()))]))
        result = audit.verify_native(data)
        self.assertEqual(result['chains'], 1);self.assertFalse(result['timing_qualified'])
        del data['graded_stats']['slices'][0]['timing']['joint_working_set']['first_invocation']['solves']
        with self.assertRaises(ValueError):audit.verify_native(data)


if __name__ == '__main__':unittest.main()
