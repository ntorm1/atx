"""Synthetic known-answer tests for backtest_integrity.py and the nav_summ v7 integrity options (platform v7 L2).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider \
     atx-impl/tools/test_backtest_integrity.py

No real data: every series and NAV dir is generated here.
"""
from __future__ import annotations

import importlib.util
import json
import math
import subprocess
from pathlib import Path

import numpy as np
import pytest

import backtest_integrity as BI
import nav_summ as NS
from test_nav_summ import SCEN, T0, DAY, write_nav, write_weights  # noqa: F401  (shared synthetic NAV writer)

BASE_BLOB = "d5fc56aa6baa4e31a9fd362c25e259e84a8297a6"  # nav_summ.py at cdc9c2a8 (platform v7 base, pre-L2)


# ------------------------------------------------------------------ PSR / MinTRL
def test_psr_known_answers():
    sr, t = 0.08, 750
    assert BI.psr(sr, sr, t, -1.0, 10.0) == pytest.approx(0.5, abs=1e-15)
    # normal returns: skew 0, kurtosis 3 -> denominator 1 + SR^2 / 2
    want = BI.norm_cdf((sr - 0.02) * math.sqrt(t - 1) / math.sqrt(1 + sr * sr / 2))
    assert BI.psr(sr, 0.02, t, 0.0, 3.0) == pytest.approx(want, rel=1e-14)
    assert BI.psr(sr, 0.0, 1, 0.0, 3.0) is None
    assert BI.psr(1.0, 0.0, 100, 5.0, 3.0) is None  # 1 - 5 + 0.5 < 0: undefined, never a number


def test_min_trl_literature_example_and_consistency():
    """literature-v7 S5: SR 1.18 ann, skew -1.32, kurtosis 13.5 -> ~550 days vs SR* 0 and ~1,640 vs SR* .5."""
    sr = 1.18 / math.sqrt(252)
    t0 = BI.min_trl(sr, 0.0, -1.32, 13.5)
    t5 = BI.min_trl(sr, 0.5 / math.sqrt(252), -1.32, 13.5)
    assert 540 < t0 < 555 and 1630 < t5 < 1660, (t0, t5)
    # at T = MinTRL the PSR is exactly 1 - alpha
    assert BI.psr(sr, 0.0, t0, -1.32, 13.5) == pytest.approx(0.95, abs=1e-9)
    assert BI.min_trl(sr, sr, 0.0, 3.0) is None and BI.min_trl(sr, sr + 0.01, 0.0, 3.0) is None
    rep = BI.psr_report(np.r_[np.full(50, 0.002), np.full(50, -0.001)], (0.0, 0.5))
    assert [b["sr_star_annual"] for b in rep["benchmarks"]] == [0.0, 0.5]
    assert rep["benchmarks"][0]["min_trl_years"] == pytest.approx(rep["benchmarks"][0]["min_trl_sessions"] / 252)


def test_lo_null_equals_nav_summ_single_dir_rule():
    rng = np.random.default_rng(3)
    x = 0.0004 + 0.003 * rng.standard_t(4, size=754)
    m = NS.net_moments(x)
    legacy = NS.dsr_rows([m], 29)[0]
    lo = BI.lo_null_dsr(m, 29)
    assert lo["dsr"] == legacy["dsr"] and lo["sr0_daily"] == legacy["sr0_daily"]
    assert lo["variance_source"] == legacy["variance_source"]
    assert BI.moments(x) == m


# ------------------------------------------------------------------ CSCV PBO
def test_cscv_vectorised_equals_literal_reference():
    rng = np.random.default_rng(11)
    r = 0.001 * rng.normal(size=(8 * 12 + 3, 5)) + np.array([0.0, 1e-4, 2e-4, -1e-4, 5e-5])
    got = BI.cscv_pbo(r, blocks=8)
    ref = BI.cscv_pbo_reference(r, blocks=8)
    assert got["splits"] == got["splits_total"] == math.comb(8, 4) == len(ref)
    np.testing.assert_allclose(got["logits"], ref, rtol=0, atol=1e-12)
    assert got["pbo"] == pytest.approx(np.mean(np.array(ref) <= 0), abs=0)
    assert got["dropped_tail_sessions"] == 3 and got["block_width"] == 12 and got["exhaustive"]
    dist = got["logit_distribution"]
    assert sum(dist["splits"]) == got["splits"] and len(dist["logit"]) == 5
    for k, c in enumerate(dist["splits"]):  # every split's logit is the logit of its rank
        assert c == int(np.sum(np.isclose(got["logits"], dist["logit"][k], rtol=0, atol=1e-12)))


def test_cscv_iid_cells_pbo_near_half_and_dominant_cell_zero():
    pbos = []
    for seed in range(4):
        rng = np.random.default_rng(100 + seed)
        pbos.append(BI.cscv_pbo(rng.normal(size=(16 * 40, 10)))["pbo"])
    assert 0.35 < np.mean(pbos) < 0.65, pbos            # iid: the IS winner's OOS rank is uniform -> PBO ~ 1/2
    rng = np.random.default_rng(7)
    r = rng.normal(size=(16 * 40, 10))
    r[:, 6] += 0.6                                       # strictly dominant in every block
    res = BI.cscv_pbo(r)
    assert res["pbo"] == 0.0 and res["splits"] == 12870 and res["winner_counts"][6] == 12870
    assert res["prob_winner_oos_loss"] == 0.0 and res["logit_mean"] > 0


def test_cscv_seeded_subsample_is_flagged_and_deterministic():
    rng = np.random.default_rng(5)
    r = rng.normal(size=(16 * 20, 6))
    a = BI.cscv_pbo(r, max_splits=500, seed=9)
    b = BI.cscv_pbo(r, max_splits=500, seed=9)
    assert a["splits"] == 500 and a["splits_total"] == 12870 and not a["exhaustive"] and a["seed"] == 9
    np.testing.assert_array_equal(a["logits"], b["logits"])
    full = BI.cscv_pbo(r)
    assert full["exhaustive"] and full["seed"] is None
    with pytest.raises(ValueError):
        BI.cscv_pbo(r, blocks=15)
    with pytest.raises(ValueError):
        BI.cscv_pbo(np.full((64, 4), np.nan))


# ------------------------------------------------------------------ ONC / effective N
def test_silhouette_matches_hand_calculation():
    x = np.array([[0.0], [1.0], [10.0], [11.0], [30.0]])
    s = BI.silhouette_samples(x, np.array([0, 0, 1, 1, 2]))
    assert s[0] == pytest.approx((10.5 - 1.0) / 10.5)
    assert s[2] == pytest.approx((min(9.5, 20.0) - 1.0) / 9.5)
    assert s[4] == 0.0                                   # a singleton's silhouette is 0 (sklearn)


def block_series(sizes, t=1500, noise=0.35, seed=1, drift=None):
    rng = np.random.default_rng(seed)
    cols, truth = [], []
    for g, n in enumerate(sizes):
        f = rng.normal(size=t)
        for _ in range(n):
            cols.append(f + noise * rng.normal(size=t) + (0.0 if drift is None else drift[g]))
            truth.append(g)
    return 0.001 * np.column_stack(cols), truth


def test_onc_recovers_block_structure_deterministically():
    r, truth = block_series([4, 5, 6, 3])
    names = [f"s{i}" for i in range(r.shape[1])]
    eff = BI.effective_trials(r, names, seed=4)
    assert eff["n_eff"] == 4
    want = sorted(sorted(n for n, g in zip(names, truth) if g == k) for k in range(4))
    assert sorted(sorted(c) for c in eff["clusters"]) == want
    again = BI.effective_trials(r, names, seed=4)
    assert again["clusters"] == eff["clusters"] and again["variance_sr"] == eff["variance_sr"]
    assert BI.effective_trials(r[:, :2], names[:2])["n_eff"] == 2      # < 3 series: each its own cluster


def test_effective_n_dsr_consistent_with_cluster_variance():
    r, _ = block_series([6, 6, 5], drift=[0.3, 0.05, -0.1], seed=8)
    names = [f"c{i}" for i in range(r.shape[1])]
    eff = BI.effective_trials(r, names)
    k = eff["n_eff"]
    srs = [BI.moments(BI.ivp_series(r, [names.index(m) for m in cl]))["sr_daily"] for cl in eff["clusters"]]
    assert eff["cluster_sr_daily"] == srs and eff["variance_sr"] == pytest.approx(np.var(srs, ddof=1), rel=1e-15)
    cell = BI.moments(r[:, 0])
    row = BI.effective_n_dsr(cell, eff)
    sr0 = BI.expected_max_sr(eff["variance_sr"], k)
    assert row["sr0_daily"] == sr0 and row["n_eff"] == k
    assert row["dsr"] == BI.deflated_sharpe(cell["sr_daily"], cell["sessions"], cell["skew"], cell["kurtosis"], sr0)
    # identical cells collapse: 20 near-copies of one series -> far fewer clusters than trials
    rng = np.random.default_rng(2)
    base = rng.normal(size=800)
    dup = np.column_stack([base + 0.05 * rng.normal(size=800) for _ in range(20)])
    assert BI.effective_trials(dup, [str(i) for i in range(20)])["n_eff"] < 20


# ------------------------------------------------------------------ ledger
def make_cells(tmp_path, k=4, t=300, seed=0):
    rng = np.random.default_rng(seed)
    dirs = []
    for i in range(k):
        nets = 0.0003 * (i + 1) + 0.01 * rng.normal(size=t)
        dirs.append(str(write_nav(tmp_path / f"cell{i}", list(nets), net_sharpe=0.1 * i)))
    return dirs


def test_ledger_append_is_idempotent_and_counts_by_kind_and_window(tmp_path):
    dirs = make_cells(tmp_path, k=3)
    ledger = tmp_path / "build-equity" / "trials.jsonl"
    recs = []
    for d in dirs:
        daily = NS.load_daily(Path(d), SCEN)
        recs.append(BI.ledger_record("construction", d, Path(d) / "summary.json", Path(d) / f"daily_{SCEN}.csv",
                                     SCEN, NS.net_series(daily), 1.0))
    added, skipped = BI.ledger_append(ledger, recs)
    assert len(added) == 3 and not skipped
    added, skipped = BI.ledger_append(ledger, recs[:2])          # identity re-run: no new trial
    assert not added and len(skipped) == 2
    import shutil
    twin = shutil.copytree(dirs[0], Path(dirs[0]).parent / "cell0-r7")   # same bytes under a suffixed cell name
    rec = BI.ledger_record("construction", str(twin), twin / "summary.json", twin / f"daily_{SCEN}.csv", SCEN,
                           NS.net_series(NS.load_daily(twin, SCEN)), 1.0)
    assert BI.ledger_append(ledger, [rec]) == ([], [rec])
    rows = BI.ledger_read(ledger)
    assert len(rows) == 3 and {r["kind"] for r in rows} == {"construction"}
    w = rows[0]["window"]
    assert w["label"] == "TRAIN" and w["first_session"] == "2020-01-04" and w["sessions"] == 300
    assert rows[0]["series"]["sha256"] == BI.sha256_file(Path(dirs[0]) / f"daily_{SCEN}.csv")
    assert rows[0]["pins"]["composition_weights_sha256"] == "0" * 64 and "summary_sha256" in rows[0]["pins"]
    adm = dict(recs[0], kind="admission", count=5, trial_id="adm-1")
    BI.ledger_append(ledger, [adm])
    counts = BI.ledger_counts(BI.ledger_read(ledger))
    key = f"TRAIN {w['first_session']}..{w['last_session']}"
    assert counts == {"construction": {key: 3}, "admission": {key: 5}}
    text = BI.appendix_a(BI.ledger_read(ledger), "trials.jsonl")
    assert text[0] == "Appendix A (trial ledger trials.jsonl): 8 trials in 4 ledger lines"
    with pytest.raises(ValueError):
        BI.ledger_record("grid", dirs[0], Path(dirs[0]) / "summary.json", Path(dirs[0]) / f"daily_{SCEN}.csv",
                         SCEN, NS.net_series(NS.load_daily(Path(dirs[0]), SCEN)), 1.0)


def test_ledger_refuses_post_train_sessions_and_changed_series(tmp_path):
    # TRAIN comes from research_window.py (v8 V-1). The window's TRAIN end can equal its seal, and nav_summ's CSV
    # reader refuses sealed sessions before any statistic, so the crossing series is built as a nets map here.
    rw = BI.research_window()
    d = make_cells(tmp_path, k=1)[0]
    paths = (Path(d) / "summary.json", Path(d) / f"daily_{SCEN}.csv", SCEN)
    for nets in ({rw.TRAIN_END_NS - DAY: 0.001, rw.TRAIN_END_NS: 0.001},        # reaches the TRAIN end
                 {rw.TRAIN_BEGIN_NS - DAY: 0.001, rw.TRAIN_BEGIN_NS: 0.001}):   # starts before TRAIN
        with pytest.raises(ValueError, match="only TRAIN"):
            BI.ledger_record("construction", d, *paths, nets, 1.0)
    w = BI.window_of([rw.TRAIN_BEGIN_NS, rw.TRAIN_END_NS - DAY])
    assert w["label"] == "TRAIN" and w["sessions"] == 2
    three_years = write_nav(tmp_path / "long", list(np.full(1090, 0.001)))   # 2020-01-02 + 1091 days: ends 2022
    rec = BI.ledger_record("construction", str(three_years), three_years / "summary.json",
                           three_years / f"daily_{SCEN}.csv", SCEN, NS.net_series(NS.load_daily(three_years, SCEN)),
                           1.0)
    # a series ending before the TRAIN end stays valid (the legacy 3-year cells)
    assert rec["window"]["last_session"] < BI.session_date(rw.TRAIN_END_NS).isoformat()
    ledger = tmp_path / "t.jsonl"
    rec = BI.ledger_record("construction", d, Path(d) / "summary.json", Path(d) / f"daily_{SCEN}.csv", SCEN,
                           NS.net_series(NS.load_daily(Path(d), SCEN)), 1.0)
    BI.ledger_append(ledger, [rec])
    with (Path(d) / f"daily_{SCEN}.csv").open("a", encoding="utf-8") as f:
        f.write("\n")
    with pytest.raises(ValueError, match="no longer matches"):
        BI.ledger_net_series(BI.ledger_read(ledger), lambda r: {})


# ------------------------------------------------------------------ nav_summ CLI
def test_nav_summ_integrity_options_end_to_end(tmp_path, capsys, monkeypatch):
    monkeypatch.chdir(tmp_path)
    dirs = [str(Path(d).relative_to(tmp_path)) for d in make_cells(tmp_path, k=5, t=16 * 20)]
    ledger = "trials.jsonl"
    assert NS.main(dirs + ["--dsr-n", "5", "--ledger", ledger, "--effective-n", "dirs", "--psr", "--pbo",
                           "--pbo-json", "pbo.json", "--ledger-n", ledger, "--json", "out.json"]) == 0
    out = capsys.readouterr().out.splitlines()
    assert "== ledger trials.jsonl: appended 5, skipped 0 already ledgered (kind construction)" in out
    assert sum(1 for line in out if line.startswith("   effective-N DSR (ONC N_eff=")) == 5
    assert sum(1 for line in out if line.startswith("   PSR vs SR* +0.00 ann:")) == 5
    assert any(line.startswith("== CSCV PBO (Bailey-Borwein-Lopez de Prado-Zhu 2017) over 5 cells: PBO ") and
               "12870 of 12870 splits (exhaustive)" in line for line in out)
    assert "Appendix A (trial ledger trials.jsonl): 5 trials in 5 ledger lines" in out
    rows = json.loads(Path("out.json").read_text())
    for r in rows:
        e, lo, q = r["deflated_effective_n"], r["deflated_lo_null"], r["deflated"]
        assert e["n_trials"] == 5 and lo["n"] == 5 and q["n"] == 5 and r["ledger"]["appended"]
        assert lo["dsr"] == BI.lo_null_dsr(r["net_moments"], 5)["dsr"]
    pbo = json.loads(Path("pbo.json").read_text())
    assert len(pbo["logits"]) == 12870 and pbo["cells"] == dirs
    # the ledger as the trial source gives the same N_eff as the dirs themselves
    assert NS.main([dirs[-1], "--dsr-n", "5", "--effective-n", ledger, "--json", "led.json"]) == 0
    capsys.readouterr()
    one = json.loads(Path("led.json").read_text())[0]["deflated_effective_n"]
    assert one["n_eff"] == rows[-1]["deflated_effective_n"]["n_eff"] and one["dsr"] == rows[-1]["deflated_effective_n"]["dsr"]
    # re-running the same ledger append adds nothing; --ledger-n alone needs no dirs
    assert NS.main(dirs + ["--dsr-n", "5", "--ledger", ledger]) == 0
    assert "appended 0, skipped 5" in capsys.readouterr().out
    assert NS.main(["--ledger-n", ledger]) == 0
    assert capsys.readouterr().out.splitlines()[0] == "Appendix A (trial ledger trials.jsonl): 5 trials in 5 ledger lines"
    with pytest.raises(SystemExit):
        NS.main(dirs[:3] + ["--pbo"])                     # a grid needs >= 4 cells


def test_legacy_output_byte_identical_to_the_v7_base(tmp_path, capsys):
    """Without the v7 options every text line and JSON field equals nav_summ.py at the platform-v7 base."""
    try:
        old = subprocess.run(["git", "cat-file", "-p", BASE_BLOB], capture_output=True, check=True,
                             cwd=Path(NS.__file__).resolve().parent).stdout
    except (OSError, subprocess.CalledProcessError):
        pytest.skip("git history with the v7-base nav_summ blob is unavailable")
    (tmp_path / "old").mkdir()
    path = tmp_path / "old" / "nav_summ_v7_base.py"
    path.write_bytes(old)
    spec = importlib.util.spec_from_file_location("nav_summ_v7_base", path)
    base = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(base)
    dirs = make_cells(tmp_path, k=4, t=250, seed=3)
    w = tmp_path / "w.json"
    write_weights(w, 0.07)
    texts, rows = {}, {}
    for tag, module in (("new", NS), ("old", base)):
        out = tmp_path / f"{tag}.json"
        assert module.main(dirs + ["--weights", str(w), "--reference", dirs[0], "--draws", "150", "--dsr-n", "29",
                                   "--json", str(out)]) == 0
        texts[tag] = capsys.readouterr().out
        rows[tag] = json.loads(out.read_text())
    assert texts["new"] == texts["old"]
    for new, old_row in zip(rows["new"], rows["old"]):
        assert set(new) == set(old_row)
        for key in old_row:
            if key != "nav_summ_run":
                assert json.dumps(new[key], sort_keys=True) == json.dumps(old_row[key], sort_keys=True), key
