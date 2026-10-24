import copy
import unittest

from greek_study import CONFIGS, SCHEMES, summarize_reference


def rows():
    result = []
    for scheme in SCHEMES:
        for label in CONFIGS:
            d = {"space-coarse": 4e-6, "center": 1e-6, "space-fine": .25e-6,
                 "time-coarse": 4e-6, "time-fine": .25e-6, "domain-wide": 1.01e-6}[label]
            result.append(dict(scheme=scheme, label=label, status="computed", result=dict(
                regime="continuation", delta=-.4+d, gamma=.02+d,
                stencil_values=[4., 4.1, 4.2], log_spacing=.001)))
    return result


class GreekStudyTests(unittest.TestCase):
    def test_fixed_center_envelope_and_both_reserves(self):
        answer = summarize_reference(dict(S=100.), rows())
        for field in ("delta", "gamma"):
            self.assertEqual(answer[field]["status"], "empirically_resolved")
            self.assertAlmostEqual(answer[field]["center"], (-.4 if field == "delta" else .02)+1e-6)
            self.assertGreater(answer[field]["uncertainty"], 6.01e-6)
            self.assertLess(answer[field]["uncertainty"], 6.011e-6)
            self.assertTrue(all(answer[field]["reserve_pass"].values()))

    def test_refused_missing_duplicate_and_mixed_results_cannot_shrink_denominator(self):
        for mutation in ("refused", "missing", "duplicate", "mixed", "nonfinite", "bad_spacing", "bad_stencil"):
            source = copy.deepcopy(rows())
            if mutation == "refused": source[0]["status"] = "refused"
            if mutation == "missing": source.pop()
            if mutation == "duplicate": source[-1] = copy.deepcopy(source[0])
            if mutation == "mixed": source[0]["result"]["regime"] = "mixed"
            if mutation == "nonfinite": source[0]["result"]["gamma"] = float("nan")
            if mutation == "bad_spacing": source[0]["result"]["log_spacing"] = 0.
            if mutation == "bad_stencil": source[0]["result"]["stencil_values"][1] = float("nan")
            answer = summarize_reference(dict(S=100.), source)
            self.assertEqual(answer["gamma"]["status"], "unresolved", mutation)

    def test_ratio_and_domain_uncertainty_fail_even_when_centers_agree(self):
        source = rows()
        next(x for x in source if x["label"] == "space-fine")["result"]["delta"] = -.4+.01
        self.assertEqual(summarize_reference(dict(S=100.), source)["delta"]["reason"], "refinement_ratio")
        source = rows()
        next(x for x in source if x["label"] == "domain-wide")["result"]["gamma"] = 1.
        answer = summarize_reference(dict(S=100.), source)
        self.assertEqual(answer["gamma"]["reason"], "reference_reserve")
        self.assertFalse(answer["gamma"]["reserve_pass"]["stress"])


if __name__ == "__main__":
    unittest.main()
