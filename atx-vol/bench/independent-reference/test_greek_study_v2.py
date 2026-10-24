import copy
import math
import unittest

from greek_study import closed_form_control, study_spec, summarize_reference
from test_greek_study import rows as original_rows


MODEL = dict(id="zero_rate_closed_form-put-03", S=100., K=100., T=1.,
             sigma=.2, r=0., q=0., side="put")
EXACT = dict(price=7.965567455405804, delta=-.460172162722971,
             gamma=.01984762737385059)


def rows():
    source = [r for r in original_rows() if r["scheme"] != "cn"]
    for row in source:
        for field, old in (("delta", -.4), ("gamma", .02)):
            row["result"][field] += EXACT[field]-old
    return source


class GreekStudyV2Tests(unittest.TestCase):
    def test_fixed_second_experiment_retains_original_and_has144_requests(self):
        first_schemes, first_configs = study_spec(1)
        schemes, configs = study_spec(2)
        self.assertEqual(first_schemes, ("cn", "dirk_l", "dirk_third"))
        self.assertEqual(first_configs["center"], (4097, 512, 1.))
        self.assertEqual(schemes, ("dirk_l", "dirk_third"))
        self.assertEqual(configs, {
            "space-coarse": (4097, 512, 1.), "center": (8193, 512, 1.),
            "space-fine": (16385, 512, 1.), "time-coarse": (8193, 256, 1.),
            "time-fine": (8193, 1024, 1.), "domain-wide": (16385, 512, 2.),
        })
        self.assertEqual(12*len(schemes)*len(configs), 144)
        with self.assertRaises(ValueError):
            study_spec(3)

    def test_three_declared_analytic_controls_have_explicit_carry_guards(self):
        answer = closed_form_control(MODEL)
        self.assertIsNotNone(answer)
        for field, value in EXACT.items():
            self.assertAlmostEqual(answer[field], value, places=12)
        call = closed_form_control(dict(MODEL, id="g000-call-03", side="call"))
        self.assertIsNotNone(call)
        self.assertAlmostEqual(call["delta"]-answer["delta"], 1., places=14)
        self.assertEqual(call["gamma"], answer["gamma"])
        negative = closed_form_control(dict(MODEL, id="negative_rate_european-put-03",
                                           r=-.01, q=.02))
        self.assertTrue(all(math.isfinite(x) for x in negative.values()))
        self.assertIsNone(closed_form_control(dict(MODEL, id="other")))
        for wrong in (dict(MODEL, r=.01), dict(MODEL, side="call"),
                      dict(MODEL, id="g000-call-03", side="call", q=.01),
                      dict(MODEL, id="negative_rate_european-put-03", r=.01, q=.02)):
            with self.assertRaises(ValueError):
                closed_form_control(wrong)

    def test_analytic_agreement_is_an_additional_condition_not_a_replacement(self):
        answer = summarize_reference(MODEL, rows(), version=2)
        for field in ("delta", "gamma"):
            self.assertEqual(answer[field]["status"], "empirically_resolved")
            self.assertTrue(answer[field]["analytic_interval_pass"])
            self.assertAlmostEqual(answer[field]["analytic_value"], EXACT[field], places=12)
        missing = rows()[:-1]
        answer = summarize_reference(MODEL, missing, version=2)
        self.assertEqual(answer["delta"]["reason"], "incomplete_or_refused")
        self.assertIsNone(answer["delta"]["center"])

    def test_shared_bias_fails_independent_interval_even_when_refinement_passes(self):
        biased = rows()
        for row in biased:
            row["result"]["delta"] += .001
        answer = summarize_reference(MODEL, biased, version=2)
        self.assertEqual(answer["delta"]["reason"], "analytic_interval")
        self.assertEqual(answer["delta"]["status"], "unresolved")
        self.assertTrue(all(answer["delta"]["reserve_pass"].values()))
        self.assertFalse(answer["delta"]["analytic_interval_pass"])
        self.assertEqual(answer["gamma"]["status"], "empirically_resolved")

    def test_refusal_duplicate_old_scheme_and_regime_changes_remain_unresolved(self):
        for mutation in ("refused", "duplicate", "old_scheme", "mixed"):
            source = copy.deepcopy(rows())
            if mutation == "refused": source[0]["status"] = "refused"
            if mutation == "duplicate": source[-1] = copy.deepcopy(source[0])
            if mutation == "old_scheme": source[0]["scheme"] = "cn"
            if mutation == "mixed": source[0]["result"]["regime"] = "mixed"
            result = summarize_reference(MODEL, source, version=2)
            self.assertEqual(result["gamma"]["status"], "unresolved", mutation)


if __name__ == "__main__":
    unittest.main()
