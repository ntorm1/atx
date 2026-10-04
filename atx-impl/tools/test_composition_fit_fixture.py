#!/usr/bin/env python3
"""P9 lane D2: the committed fixture of ``atx-research-composition fit`` (ResearchComposition.FitEqualsPythonFixture,
atx-engine/tests/research/research_composition_fit_test.cpp) is the EXISTING Python fit side run on synthetic inputs
(migration plan section 3.1: the fixture holds the inputs, this generator runs today's Python on them, the pytest
re-runs it and must reproduce the committed bytes, the gtest reads the same inputs and must reproduce the values).

The inputs make every sum exact (factor values k * 2^-10 with |k| <= 200, 256 TRAIN decisions, dyadic within-theme
shares wherever theme-erc-v1 measures a covariance), so the numpy reductions (pairwise sums, BLAS dot) and the C++'s
pinned order give the same bits; one case ("exact": false, ic-shrink-v1 + theme-erc-v1) has non-dyadic shares and is
held to 1e-12 x max(1, |x|) instead. The fixture also pins numpy's pairwise summation (np.add.reduce on a vector and
mean(axis=1) on rows) against a Python transcription of the C++ port (fit.hpp numpy_pairwise_sum).

Regenerate (after reviewing why): ATX_D2_REGEN=1 "C:/Program Files/Python312/python.exe" -m pytest -q
-p no:cacheprovider atx-impl/tools/test_composition_fit_fixture.py
"""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import sys
import unittest

import numpy as np

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import composition_ic_shrink as cis  # noqa: E402
import composition_resid as cr  # noqa: E402
import composition_rules as crules  # noqa: E402
import composition_theme_erc as cerc  # noqa: E402
import composition_theme_tsmom as cts  # noqa: E402

REPO = TOOLS.parents[1]
FIXTURE = REPO / "atx-engine" / "tests" / "fixtures" / "research_composition" / "fit_v1.json"
SCHEMA = "atx.composition-fit-fixture/v1"
SPEC_SCHEMA = "atx.composition-fit-spec/v1"
DECISIONS, TRAIN_DECISIONS = 300, 256
DAY_NS = 86_400_000_000_000
FIRST_SESSION_NS = 1_577_836_800_000_000_000 + 7 * DAY_NS   # inside TRAIN (research-window-v2)
IDS = ("ear", "res_mom_12_1", "c2", "c3", "c4", "c5", "c6", "c7")
REGISTERED = ("value", "profitability_quality", "investment_issuance", "earnings_momentum", "price_momentum",
              "low_risk", "short_interest", "reversal_seasonality", "options_implied", "ownership_flow",
              "filing_events", "price_volume", "merger_arbitrage")
MISSING_REGISTRY = TOOLS / "no-such-registry.json"   # ew_theme_std: the declared tier scores, the given source tiers


class FixtureError(ValueError):
    pass


def numpy_pairwise(a: list[float]) -> float:
    """fit.hpp numpy_pairwise_sum transcribed (numpy pairwise_sum_DOUBLE, block 128, eight accumulators)."""
    n = len(a)
    if n < 8:
        res = 0.0
        for v in a:
            res += v
        return res
    if n <= 128:
        r = list(a[:8])
        i = 8
        while i < n - (n % 8):
            for j in range(8):
                r[j] += a[i + j]
            i += 8
        res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]))
        while i < n:
            res += a[i]
            i += 1
        return res
    n2 = n // 2
    n2 -= n2 % 8
    return numpy_pairwise(a[:n2]) + numpy_pairwise(a[n2:])


def factor_matrix() -> np.ndarray:
    """decisions x candidates, k * 2^-10 with |k| <= 200; candidate c7 is flat (NaN) on a few TRAIN decisions."""
    rng = np.random.default_rng(20261003)
    k = rng.integers(-200, 201, size=(DECISIONS, len(IDS)))
    drift = np.array([3, -2, 5, 1, -4, 2, 6, 0])           # distinct sleeve means and IC signs
    f = (k + drift).astype(np.float64) / 1024.0
    f[[10, 77, 140, 255], 7] = np.nan
    return f


def sessions() -> list[int]:
    return [FIRST_SESSION_NS + d * DAY_NS for d in range(DECISIONS)]


def train_mask() -> np.ndarray:
    return np.arange(DECISIONS) < TRAIN_DECISIONS


def train_mean(f: np.ndarray, column: int, mask: np.ndarray) -> float:
    """fit_composition_weights.screen_v4's train_mean (_stats): mean of the live TRAIN values (prior sign +1)."""
    x = f[:, column]
    return float(x[np.isfinite(x) & mask].mean())


def passes_json(passes, ids) -> list:
    return [{"capped": [ids[k] for k in p["capped"]], "excess_by_theme": p["excess_by_theme"]} for p in passes]


def run_case(case: dict, f: np.ndarray) -> dict:
    """The fitter's fit_prior chain for one spec, as fit_composition_weights.py runs it (rule functions unchanged)."""
    spec, mask = case["spec"], train_mask()
    members = spec["members"]
    ids = [m["id"] for m in members]
    themes = [m["theme"] for m in members]
    columns = [IDS.index(i) for i in ids]
    zero = np.where(np.isnan(f), 0.0, f)
    matrix = np.vstack([m["sign"] * zero[:, c] for m, c in zip(members, columns)])
    gains = [m["gain"] for m in members] if "gain" in members[0] else None
    out: dict = {}
    if spec["composition"] in crules.STD_RULES:
        parent = crules.ew_theme_std(ids, themes, [m["tier"] for m in members], registry_path=MISSING_REGISTRY,
                                     error=FixtureError, gains=gains)
        prov = parent.provenance
        out["parent"] = {"member_tiers": [prov["member_tiers"][i] for i in ids],
                         "member_scores": [prov["member_scores"][i] for i in ids],
                         "tier_regrades": prov["tier_regrades"]}
    else:
        ics = [train_mean(f, c, mask) for c in columns]
        parent = cis.ic_shrink(ids, themes, ics, error=FixtureError, gains=gains)
        prov = parent.provenance
        out["parent"] = {"ic": ics, "shrunk_ic": [prov["shrunk_ic"][i] for i in ids],
                         "within_theme_shares": [prov["within_theme_shares"][i] for i in ids],
                         "theme_mean_ic": prov["theme_mean_ic"], "equal_share_themes": prov["equal_share_themes"]}
    out["parent"].update(weights=[float(w) for w in parent.weights],
                         weights_before_cap=[prov["weights_before_cap"][i] for i in ids], cap=prov["cap"],
                         cap_iterations=prov["cap_iterations"])
    weights = parent.weights
    if spec.get("theme_erc"):
        erc = cerc.theme_erc(parent, ids, themes, matrix, mask, crules.STD_RULE_ID if spec["composition"] in
                             crules.STD_RULES else cis.RULE_ID, error=FixtureError)
        p = erc.provenance
        order = p["theme_order"]
        out["theme_erc"] = {"within_theme_shares": [p["within_theme_shares"][i] for i in ids], "theme_order": order,
                            "covariance": p["covariance"], "decisions": p["decisions"],
                            "theme_shares": [p["theme_shares"][t] for t in order],
                            "risk_contributions": [p["risk_contributions"][t] for t in order],
                            "dispersion": p["dispersion"], "cap": p["cap"],
                            "weights_before_cap": [p["weights_before_cap"][i] for i in ids],
                            "weights": [float(w) for w in erc.weights], "cap_iterations": p["cap_iterations"]}
        weights = erc.weights
    if spec.get("theme_tsmom"):
        s = cts.schedule(ids, themes, weights, matrix, np.array(sessions(), dtype=np.int64), mask, "parent",
                         error=FixtureError)
        b, p = s.block, s.provenance
        out["theme_tsmom"] = {
            "theme_order": b["themes"], "parent_masses": [p["parent_masses_report_only"][t] for t in b["themes"]],
            "blocks": [{"decision_index": d["decision_index"], "from_session": blk["from_session"],
                        "trailing": blk["trailing"], "masses": [d["masses_report_only"][t] for t in b["themes"]],
                        "themes_off": d["themes_off"]} for blk, d in zip(b["blocks"], p["block_detail"])],
            "theme_blocks_off": p["theme_blocks_off"],
            "theme_on_count": [round(p["theme_on_fraction"][t] * p["blocks"]) for t in b["themes"]]}
    if spec.get("theme_resid"):
        weighted = [t for t, w in zip(themes, weights) if w > 0]
        out["theme_resid"] = {"order": cr.theme_order(weighted, spec["registered_themes"], FixtureError)}
    out["weights"] = [float(w) for w in weights]
    return out


def member(i: str, theme: str, tier: str | None = None, gain: float | None = None) -> dict:
    row = {"id": i, "theme": theme, "sign": 1}
    if tier is not None:
        row["tier"] = tier
    if gain is not None:
        row["gain"] = gain
    return row


def specs() -> list[dict]:
    scores = dict(crules.TIER_SCORES_DECLARED)
    std = dict(schema=SPEC_SCHEMA, tier_scores=scores)
    return [
        {"name": "ew-theme-std-cap-regrades", "exact": True, "spec": dict(std, composition="ew-theme-std-v1", members=[
            member("c2", "value", "A"), member("ear", "value", "B+"), member("c3", "low_risk", "B"),
            member("c4", "low_risk", "B"), member("c5", "low_risk", "B"), member("res_mom_12_1", "price_momentum",
                                                                                "B-"),
            member("c6", "price_momentum", "B-")])},
        {"name": "ew-theme-std-erc-tsmom", "exact": True, "spec": dict(
            std, composition="ew-theme-std-v1", theme_erc=True, theme_tsmom=True, members=[
                member("c2", "value", "B"), member("c3", "value", "B"), member("c4", "low_risk", "B"),
                member("c5", "low_risk", "B"), member("c6", "price_momentum", "B"), member("c7", "price_momentum",
                                                                                           "B")])},
        {"name": "ew-theme-std-aim-resid", "exact": True, "spec": dict(
            std, composition="ew-theme-std-aim-v1", theme_resid=True, registered_themes=list(REGISTERED), members=[
                member("c2", "value", "A", 0.5), member("c3", "value", "B", 1.0),
                member("c4", "ownership_flow", "B", 0.75), member("c5", "ownership_flow", "B+", 0.25),
                member("c6", "low_risk", "A-", 1.0), member("c7", "low_risk", "B", 0.5),
                member("ear", "earnings_momentum", "B+", 0.625), member("res_mom_12_1", "earnings_momentum", "B-",
                                                                        0.875)])},
        {"name": "ic-shrink-tsmom", "exact": True, "spec": dict(
            schema=SPEC_SCHEMA, composition="ic-shrink-v1", theme_tsmom=True, members=[
                member("c2", "value"), member("ear", "value"), member("c3", "low_risk"), member("c4", "low_risk"),
                member("c5", "price_momentum"), member("res_mom_12_1", "price_momentum"),
                member("c6", "short_interest"), member("c7", "short_interest")])},
        {"name": "ic-shrink-aim", "exact": True, "spec": dict(
            schema=SPEC_SCHEMA, composition="ic-shrink-aim-v1", members=[
                member("c2", "value", gain=0.5), member("ear", "value", gain=1.0), member("c3", "low_risk", gain=0.25),
                member("c4", "low_risk", gain=0.75), member("c5", "price_momentum", gain=1.0),
                member("c6", "price_momentum", gain=0.125)])},
        {"name": "ic-shrink-erc-tolerance", "exact": False, "spec": dict(
            schema=SPEC_SCHEMA, composition="ic-shrink-v1", theme_erc=True, members=[
                member("c2", "value"), member("c3", "value"), member("c4", "low_risk"), member("c5", "low_risk"),
                member("c6", "price_momentum"), member("c7", "price_momentum"), member("ear", "short_interest"),
                member("res_mom_12_1", "short_interest")])},
    ]


def pairwise_cases() -> dict:
    rng = np.random.default_rng(7)
    sums = []
    for n in (5, 8, 100, 129, 1000, 1001):
        x = rng.normal(0.0, 1e-3, size=n) + 1e-4
        sums.append({"x": [float(v) for v in x], "sum": float(np.add.reduce(x))})
    rows = rng.normal(0.0, 1e-2, size=(3, 256)) + np.array([[1e-3], [-2e-3], [5e-4]])
    means = rows.mean(axis=1, keepdims=True)[:, 0]
    return {"sums": sums, "rows": [[float(v) for v in r] for r in rows], "mean_axis1": [float(m) for m in means]}


def build() -> dict:
    f = factor_matrix()
    cases = []
    for case in specs():
        cases.append(dict(case, expected=run_case(case, f)))
    factors = [[None if math.isnan(v) else float(v) for v in row] for row in f]
    return {"schema": SCHEMA, "generator": "atx-impl/tools/test_composition_fit_fixture.py",
            "inputs": {"ids": list(IDS), "decisions": DECISIONS, "factors": factors, "sessions": sessions(),
                       "train": [int(v) for v in train_mask()]},
            "cases": cases, "pairwise": pairwise_cases()}


def dump(doc: dict) -> bytes:
    return (json.dumps(doc, sort_keys=True, indent=1, allow_nan=False) + "\n").encode("utf-8")


class FitFixture(unittest.TestCase):
    def test_fixture_is_todays_python(self):
        data = dump(build())
        if os.environ.get("ATX_D2_REGEN") == "1":
            FIXTURE.parent.mkdir(parents=True, exist_ok=True)
            FIXTURE.write_bytes(data)
        self.assertTrue(FIXTURE.is_file(), "missing fixture: run with ATX_D2_REGEN=1")
        self.assertEqual(json.loads(FIXTURE.read_bytes()), json.loads(data))

    def test_numpy_reduces_pairwise_as_the_cpp_port(self):
        """np.add.reduce on a vector and mean(axis=1) on C-contiguous rows are the pairwise sum fit.hpp ports."""
        p = pairwise_cases()
        for case in p["sums"]:
            self.assertEqual(numpy_pairwise(case["x"]), case["sum"], len(case["x"]))
            seq = 0.0
            for v in case["x"]:
                seq += v
            if len(case["x"]) > 128:   # the ports are not the sequential sum (the test has teeth)
                self.assertNotEqual(seq, case["sum"], len(case["x"]))
        for row, mean in zip(p["rows"], p["mean_axis1"]):
            self.assertEqual(numpy_pairwise(row) / len(row), mean)

    def test_exact_cases_have_exact_sums(self):
        """Every factor value is k / 1024 and the ERC cases' within-theme shares are dyadic, so the covariance is the
        same in any summation order (what lets the gtest compare bits)."""
        f = factor_matrix()
        finite = f[np.isfinite(f)]
        self.assertTrue(np.all(finite * 1024 == np.round(finite * 1024)))
        doc = build()
        for case in doc["cases"]:
            erc = case["expected"].get("theme_erc")
            if case["exact"] and erc is not None:
                for a in erc["within_theme_shares"]:
                    self.assertEqual(a * 2 ** 10, round(a * 2 ** 10), case["name"])
        self.assertEqual(int(train_mask().sum()), TRAIN_DECISIONS)
        self.assertEqual(TRAIN_DECISIONS & (TRAIN_DECISIONS - 1), 0)   # a power of two: the means are exact

    def test_cases_exercise_every_stage(self):
        doc = build()
        names = {c["name"]: c["expected"] for c in doc["cases"]}
        self.assertTrue(names["ew-theme-std-cap-regrades"]["parent"]["cap_iterations"])
        statuses = {r["status"] for r in names["ew-theme-std-cap-regrades"]["parent"]["tier_regrades"]}
        self.assertTrue({"applied", "already-at-target", "not-a-member"} <= statuses)
        self.assertGreaterEqual(len(names["ew-theme-std-erc-tsmom"]["theme_tsmom"]["blocks"]), 2)
        self.assertIn("theme_resid", names["ew-theme-std-aim-resid"])
        self.assertTrue(names["ic-shrink-tsmom"]["parent"]["equal_share_themes"] or
                        any(s == 0.0 for s in names["ic-shrink-tsmom"]["parent"]["within_theme_shares"]))


if __name__ == "__main__":
    unittest.main()
