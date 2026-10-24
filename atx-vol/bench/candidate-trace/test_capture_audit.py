import copy
import struct
import unittest

import capture_audit as audit


def real(value):
    return {"value": value, "bits": struct.pack(">d", value).hex()}


def options(n, q, p, sweeps, tol, profile):
    return {"n_collocation": n, "n_quadrature": q, "n_quad_price": p,
            "max_newton_iter": sweeps, "tol": real(tol), "profile": profile}


def fixture():
    slot = {"prepared": False, "anchor": real(0.), "T": real(0.), "r": real(0.),
            "q": real(0.), "side": 1, "options": None}
    before = {"preparations": 1, "hits": 0, "slots": [copy.deepcopy(slot), {
        "prepared": True, "anchor": real(100.), "T": real(1.), "r": real(.05),
        "q": real(.01), "side": 1, "options": None}]}
    after = copy.deepcopy(before)
    after["hits"] = 1
    phases = [{"phase": i, "visited": False, "complete": False, "stopped_early": False,
               "outcome": 0, "sigma": None, "seed_sigma": None, "error": None,
               "typed_status": None, "options": None, "elapsed_ns": 0,
               "work": [0, 0, 0], "floor_preparations": 0, "floor_hits": 0} for i in range(6)]
    phases[0].update(visited=True, complete=True, outcome=1, sigma=real(.35),
                     options=options(7, 16, 48, 5, 1e-8, 2), elapsed_ns=10, work=[1, 1, 1])
    phases[1].update(visited=True, complete=True, outcome=1, sigma=real(.35),
                     seed_sigma=real(.35), elapsed_ns=10, work=[2, 2, 0], floor_hits=1)
    trace = {"complete": True, "floor_cache_present": True,
             "request": {"price": real(10.), "S": real(100.), "K": real(100.),
                         "T": real(1.), "r": real(.05), "q": real(.01),
                         "budget": real(.001), "side": 1},
             "options": {"method": 0, "tol": real(1e-7), "max_iter": 64, "al_options": None,
                         "has_correction": False, "warm_start": real(0.), "price_tol": real(0.),
                         "refuse_non_monotone": False, "strategy": 0, "seed_profile": None},
             "kernel": 1, "outcome": 2, "sigma": real(.35), "stats": {
                 "stopped_early": False, "accepted_early": False, "retried_full_search": False,
                 "screen_attempted": True, "screen_stopped_early": False, "screen_accepted": True},
             "phases": phases, "elapsed_ns": 25, "work": [3, 3, 1],
             "floor_preparations": 0, "floor_hits": 1, "floor_before": before, "floor_after": after}
    counters = {"reimply_candidate_attempts": 1, "reimply_candidate_roots": 1,
                "reimply_screen_attempts": 1, "reimply_screen_stops": 0, "reimply_screen_roots": 1,
                "reimply_proposal_stops": 0, "reimply_proposal_roots": 0, "reimply_proposal_retries": 0}
    return {"schema": "atx-price-candidate-capture-v1", "timing_qualifying": False,
            "capture_enabled": True, "valid": True, "attempted_calls": 1,
            "record_capacity": 4, "cache_capacity": 4, "record_overflow": False, "cache_overflow": False,
            "phase_order": audit.PHASES, "records": [{"sequence": 0, "cache_id": 1, "trace": trace}],
            "caches": [{"id": 1, "address": 1234, "closed": True, "created_before_call": 0,
                        "closed_after_calls": 1, "final_state": copy.deepcopy(after)}],
            "slice_counters": {"slices": [{"timing": counters}]}}


class CaptureAudit(unittest.TestCase):
    def test_complete_one_call_retains_all_counts(self):
        result = audit.validate_capture(fixture())
        self.assertEqual(result["attempts"], 1)
        self.assertEqual(result["accepted"], 1)
        self.assertEqual(result["refused"], 0)
        self.assertEqual(result["phase_calls"], [1, 1, 0, 0, 0, 0])

    def test_refusal_retry_invalid_and_allocation_paths_keep_denominators(self):
        for kind in ("refused", "retry", "typed_refusal", "invalid", "allocation"):
            with self.subTest(kind=kind):
                value = fixture()
                trace = value["records"][0]["trace"]
                old = copy.deepcopy(trace["phases"])
                trace["phases"] = [dict(copy.deepcopy(old[2]), phase=i) for i in range(6)]
                phases = trace["phases"]
                trace["stats"] = {k: False for k in trace["stats"]}
                trace["outcome"], trace["sigma"] = 3, None
                trace["floor_after"] = copy.deepcopy(trace["floor_before"])
                if kind == "invalid":
                    trace["request"]["budget"] = real(0.)
                    trace["outcome"] = 1
                elif kind == "allocation":
                    phases[0] = dict(old[0], outcome=4, sigma=None)
                    trace["outcome"] = 4
                    trace["stats"]["screen_attempted"] = True
                elif kind == "refused":
                    phases[0] = dict(old[0], outcome=3, sigma=None, error=0)
                    phases[2] = dict(old[0], phase=2, outcome=3, sigma=None, error=0,
                                     options=options(12, 24, 48, 6, 1e-10, 1))
                    trace["stats"]["screen_attempted"] = True
                else:
                    trace["kernel"] = 0
                    phases[2] = dict(old[0], phase=2, stopped_early=True,
                                     options=options(12, 24, 48, 6, 1e-10, 1))
                    phases[3] = dict(old[1], phase=3, outcome=2, sigma=None, floor_hits=0)
                    phases[4] = dict(phases[2], phase=4, stopped_early=False, sigma=real(.36), typed_status=0)
                    trace["stats"].update(stopped_early=True, retried_full_search=True)
                    if kind == "retry":
                        phases[5] = dict(old[1], phase=5, sigma=real(.36), seed_sigma=real(.36))
                        trace["outcome"], trace["sigma"] = 2, real(.36)
                        trace["floor_after"]["hits"] = 1
                    else:
                        phases[4].update(outcome=2, sigma={"value": None, "bits": "7ff8000000000000"}, typed_status=5)
                trace["work"] = [sum(p["work"][i] for p in phases) for i in range(3)]
                trace["elapsed_ns"] = 1 + sum(p["elapsed_ns"] for p in phases)
                trace["floor_hits"] = sum(p["floor_hits"] for p in phases)
                value["caches"][0]["final_state"] = copy.deepcopy(trace["floor_after"])
                counts = value["slice_counters"]["slices"][0]["timing"]
                counts.update(reimply_candidate_roots=int(kind == "retry"), reimply_screen_roots=0,
                              reimply_screen_attempts=int(kind in ("refused", "allocation")),
                              reimply_proposal_stops=int(kind in ("retry", "typed_refusal")),
                              reimply_proposal_retries=int(kind in ("retry", "typed_refusal")))
                result = audit.validate_capture(value)
                self.assertEqual(result["attempts"], 1)
                self.assertEqual(result["accepted"], int(kind == "retry"))
                self.assertEqual(result["refused"], int(kind != "retry"))

    def test_incomplete_or_fabricated_evidence_is_rejected(self):
        def record(x): return x["records"][0]
        mutations = [
            lambda x: x.update(attempted_calls=2),
            lambda x: x.update(record_overflow=True),
            lambda x: x.update(timing_qualifying=True),
            lambda x: record(x).update(sequence=1),
            lambda x: record(x).update(cache_id=2),
            lambda x: x["caches"][0].update(closed=False),
            lambda x: x["caches"][0].update(closed_after_calls=0),
            lambda x: record(x)["trace"].update(complete=False),
            lambda x: record(x)["trace"]["phases"][0].update(complete=False),
            lambda x: record(x)["trace"]["phases"][2].update(visited=True, complete=True),
            lambda x: record(x)["trace"]["phases"][0]["options"].update(profile=1),
            lambda x: record(x)["trace"]["phases"][1].update(seed_sigma=real(.36)),
            lambda x: record(x)["trace"]["stats"].update(screen_accepted=False),
            lambda x: record(x)["trace"].update(work=[0, 0, 0]),
            lambda x: record(x)["trace"].update(elapsed_ns=1),
            lambda x: record(x)["trace"]["floor_after"].update(hits=2),
            lambda x: record(x)["trace"]["request"]["S"].update(value=101.),
            lambda x: record(x)["trace"]["request"]["S"].update(value=True),
            lambda x: x["slice_counters"]["slices"][0]["timing"].update(reimply_candidate_roots=0),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                value = fixture()
                mutate(value)
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    audit.validate_capture(value)


if __name__ == "__main__":
    unittest.main()
