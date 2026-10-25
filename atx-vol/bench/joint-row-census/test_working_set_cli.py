"""Small generated same-model CLI controls, never the SPY candidate."""
import json
import unittest

import test_census_cli as helper
import working_set_audit as audit


class WorkingSetCli(unittest.TestCase):
    setUp = helper.CensusCliTests.setUp
    invoke = helper.CensusCliTests.invoke

    def test_enabled_work_is_owned_once_and_provenance_is_explicit(self):
        result = self.invoke('working')
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        summary = audit.verify_native(report)
        self.assertEqual(summary['chains'], 1)
        self.assertEqual(summary['totals']['invocations'], 1)
        self.assertEqual(summary['totals']['slot_detail_invocations_omitted'], 0)
        mode = report['bench']['build_info']['cold_seed_target']['joint_constraints']
        self.assertIs(mode['enabled'], True)
        self.assertEqual(mode['selection'], 'BoundedWorkingSet')
        self.assertEqual(mode['cmake_option'], 'ATX_VOL_JOINT_WORKING_SET_RESEARCH')
        self.assertEqual(mode['maximum_reduced_solves'], 3)
        self.assertEqual(mode['maximum_full_fallback_solves'], 1)
        self.assertEqual(report['rows'], 52)

    def test_full_census_cannot_mislabel_working_set_matrix(self):
        result = self.invoke('census', ['--joint-row-census', str(self.root/'census.json')])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root/'census.csv').exists())
        self.assertFalse((self.root/'census.json').exists())


if __name__ == '__main__':unittest.main()
