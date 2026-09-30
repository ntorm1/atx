"""Characteristics v2 PIT harness (Task 4): recomputing a bucket from rows <= d reproduces the build exactly, and a
planted look-ahead feature (``lead``, whole-history mean) is caught."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.alpha_panel import char_registry as R
from atx_db.alpha_panel import characteristics as CH
from atx_db.alpha_panel import common as C
from tests.test_alpha_panel_characteristics import NB, make_lake

D = dt.date
SESSIONS = (D(2019, 3, 6), D(2019, 6, 14), D(2019, 11, 29))
PLANTED = {"lead_r": "lead(r) OVER (PARTITION BY security_id ORDER BY session_date)",
           "full_mean_r": "avg(r) OVER (PARTITION BY security_id)"}
PLANTED_FEATURES = [
    R.Feature("x_lead_ret", "momentum", "lead_r", 1, "planted look-ahead", ("panel.ret",), 1),
    R.Feature("x_demeaned_ret", "reversal", "r - full_mean_r", 1, "planted whole-history mean", ("panel.ret",), 1),
]


def _build(root, monkeypatch, features=None, extra=None):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(root))
    make_lake(root)
    CH.run(["extract", "line", "year", "manifest"], [2019], nb=NB, memory="300MB", threads=1,
           features=features, extra_terms=extra)


def test_pit_harness_reproduces_the_build(tmp_path, monkeypatch):
    _build(tmp_path, monkeypatch)
    con = C.connect(memory="300MB", threads=1, db_file="pit_test.duckdb")
    try:
        for d in SESSIONS:
            for b in range(NB):
                res = CH.pit_check(con, b, d, nb=NB)
                assert res["pass"], (d, b, res["mismatches"])
                assert res["rows_recomputed"] == res["rows_built"] > 0
                # the comparison is not vacuous: most features carry values on these sessions
                assert sum(1 for v in res["non_null_compared"].values() if v > 0) >= 80
    finally:
        con.close()
    summary = CH.run_pit([SESSIONS[1]], [0, 1], nb=NB, memory="300MB", threads=1, out=tmp_path / "pit.json")
    assert summary["pass"] and (tmp_path / "pit.json").exists()


def test_pit_harness_catches_planted_lookahead(tmp_path, monkeypatch):
    feats = list(R.REGISTRY) + PLANTED_FEATURES
    _build(tmp_path, monkeypatch, feats, PLANTED)
    con = C.connect(memory="300MB", threads=1, db_file="pit_test.duckdb")
    try:
        for d in SESSIONS[1:]:
            res = CH.pit_check(con, 0, d, nb=NB, features=feats, extra_terms=PLANTED)
            assert not res["pass"]
            assert set(res["mismatches"]) == {"x_lead_ret", "x_demeaned_ret"}, res["mismatches"]
            assert res["mismatches"]["x_lead_ret"]["count"] == res["rows_built"]
    finally:
        con.close()


def test_build_refuses_lookahead_in_production_sql(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    bad = R.Feature("x_bad", "momentum", "lead(close) OVER (PARTITION BY security_id ORDER BY session_date)", 1, "c",
                    ("panel.close",), 1)
    with pytest.raises(ValueError):
        CH.run(["year"], [2019], features=[*R.REGISTRY, bad])
    assert not (tmp_path / "characteristics").exists()
