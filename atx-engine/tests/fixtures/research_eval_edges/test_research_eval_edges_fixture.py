"""The Python side of the eval verb's edge-case fixture (P9 lane B2 fix round 1; gtest EvalEdges.*).

Pins expected.json to today's Python statistics of record on the committed parameters, so the engine is tied to them
under rulings B2-R1 (paired conditioning), B2-R2 (constant CSCV candidate) and B2-9 (house DSR rows for any N).

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/research_eval_edges/test_research_eval_edges_fixture.py
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import generate_research_eval_edges as G  # noqa: E402

EXPECTED = json.loads((HERE / G.EXPECTED).read_bytes())


def test_the_committed_values_are_todays_python():
    assert G.check() == []


def test_each_case_sits_at_its_stated_one_minus_rho():
    for c in EXPECTED["paired"]["cases"]:
        if c["target_one_minus_rho"] is None:
            assert c["name"] == "self" and c["eps"] == 0.0 and c["one_minus_rho"] == 0.0
            continue
        assert abs(c["one_minus_rho"] / c["target_one_minus_rho"] - 1.0) < 1e-9, c["name"]
        assert c["degenerate"] == (c["one_minus_rho"] < G.DEGENERATE), c["name"]
    names = {c["name"]: c for c in EXPECTED["paired"]["cases"]}
    assert not names["omr_1.02e-6"]["degenerate"] and names["omr_0.98e-6"]["degenerate"]


def test_the_generator_is_exact_ieee():
    """Each series value is one IEEE double operation chain the gtest repeats: spot-check against integer math."""
    u = G.uniforms(G.A_SEED, 3)
    s = G.A_SEED
    for k in range(3):
        s = (s * G.LCG_MUL + G.LCG_ADD) % 2 ** 64
        assert u[k] == (s >> 11) / 2 ** 53
    a = G.reference()
    assert a.dtype == np.float64 and a.size == G.T
    assert np.array_equal(G.cell(0.0, 0.0), a)


def test_the_house_dsr_grid_spans_n_2_to_a_million():
    """Ruling B2-9: the house DSR tie runs N from 2 to 1e6, every row defined, the DSR spread over (0, 1)."""
    h = EXPECTED["house_dsr"]
    assert h["n"] == list(G.HOUSE_N) and h["n"][0] == 2 and h["n"][-1] == 10 ** 6
    assert [q["n"] for q in h["norm_ppf"]] == h["n"] and [v["n"] for v in h["verb"]] == h["n"]
    dsr = [r["dsr"] for m in h["direct"] for r in m["rows"]]
    assert len(dsr) == 3 * len(h["n"]) and all(d is not None and 0.0 < d < 1.0 for d in dsr)
    assert min(dsr) < 0.01 and max(dsr) > 0.9
    for m in h["direct"]:   # SR* grows with N
        sr0 = [r["sr0_daily"] for r in m["rows"]]
        assert sr0 == sorted(sr0) and sr0[0] > 0.0


def test_the_cscv_grid_has_one_constant_candidate_that_never_wins():
    py = EXPECTED["cscv"]["python"]
    m = G.cscv_matrix()
    assert m.shape == (G.CSCV_T, len(G.CSCV_DRIFTS) + 1)
    assert np.all(m[:, -1] == G.CSCV_CONSTANT) and all(np.ptp(m[:, c]) > 0 for c in range(m.shape[1] - 1))
    assert py["winner_counts"][-1] == 0 and 0.0 < py["pbo"] < 1.0
