"""nav_summ --pool (platform v8 H-1): era pooling on the tiny_world fixture split in two eras.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_nav_summ_pool.py

No real data. The NAV dirs are written here: the sessions are tiny_world's scored sessions (2021-12 .. 2022-12, inside
TRAIN) and the daily net is the book of tiny_world's two planted states on its simulated returns (scripts/tests/
fixtures/tiny_world.py). One CSV of the whole span carries a flat row plus a re-deployment row at the split, so two era
CSVs cut at that session boundary hold exactly its rows: every return-row and turnover statistic of the pool equals the
single CSV's, and only the post-ramp rows (63 per era) differ. History-era tests use synthetic 2014 sessions.
"""
from __future__ import annotations

import csv
import datetime as dt
import json
import math
from pathlib import Path
import sys

import numpy as np
import pytest

import backtest_integrity as BI
import nav_summ as NS
from test_nav_summ import COLUMNS, SCEN

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "tests" / "fixtures"))
sys.path.insert(0, str(REPO / "scripts"))
import research_ledger as RL  # noqa: E402
import tiny_world as TW  # noqa: E402

DAY = 86_400_000_000_000
SAME_KEYS = ("mean_gross_leverage", "mean_net_leverage", "mean_abs_net_leverage", "mean_gross_leverage_all_rows",
             "mean_net_leverage_all_rows", "csv_rows", "mean_held_names", "tau_gmv_mean", "tau_gmv_p95",
             "tau_gmv_sessions", "cost_per_gmv_turnover", "cost_bps_traded", "return_rows", "net_moments")


# ------------------------------------------------------------------ the tiny_world NAV rows
def tiny_rows(split: int | None = None) -> dict:
    """Daily NAV columns over tiny_world's scored sessions: row 0 flat, row 1 the deployment, then return rows; with
    ``split`` the rows split and split + 1 repeat that pattern (a flat row and a re-deployment)."""
    w = TW.simulate(TW.DEFAULT_SEED)
    sessions = np.array([TW.session_ns(d) for d in w["sessions"]], dtype=np.int64)[TW.SCORE_BEGIN:]
    close = w["close"][TW.SCORE_BEGIN - 1:]
    ret = close[1:] / close[:-1] - 1.0                           # r(t) of each scored session
    state = (w["za"] + w["zb"])[TW.SCORE_BEGIN - 1:-1]            # the planted states known before t
    book = state - state.mean(axis=1, keepdims=True)
    book /= np.abs(book).sum(axis=1, keepdims=True)
    net = (book * ret).sum(axis=1)
    t = sessions.size
    starts = [0] if split is None else [0, split]
    flat = np.zeros(t, dtype=bool)
    deploy = np.zeros(t, dtype=bool)
    for s in starts:
        flat[s], deploy[s + 1] = True, True
    k = np.arange(t)
    return {"session_index": 399 + k, "session_ns": sessions, "executed": (~flat).astype(int),
            "return_observation": (~flat & ~deploy).astype(int), "net_return": np.where(flat | deploy, 0.0, net),
            "gross_return": np.where(flat | deploy, 0.0, net + 1e-5), "trade_cost_return": np.zeros(t),
            "traded_dollars": np.where(flat, 0.0, np.where(deploy, 5e8, 1e7 + 1e5 * (k % 7))),
            "trade_cost_dollars": np.where(flat, 0.0, np.where(deploy, 1e5, 2e3 + 10.0 * (k % 5))),
            "pretrade_gross_dollars": np.where(flat | deploy, 0.0, 1e9),
            "gross_leverage": np.where(flat, 0.0, 1.0 + 0.1 * np.sin(k / 9.0)),
            "net_leverage": np.where(flat, 0.0, 0.02 * np.cos(k / 7.0)), "held_names": 60 + (k % 5),
            "one_way_turnover_gmv": np.where(flat, np.nan, 0.05 + 0.01 * (k % 3)), "neutralize": ["applied"] * t,
            "pretrade_nav": np.full(t, 1e9)}


def write_dir(d: Path, rows: dict, lo: int = 0, hi: int | None = None, *, role_sha: str = "ab" * 32,
              weights_sha: str = "0" * 64, rule: str = "aim-partial-v5", scenario: str = SCEN,
              net_sharpe=0.5) -> Path:
    """A NAV dir (summary.json + daily CSV, strategy_nav_replay's column names) of rows[lo:hi]."""
    hi = len(rows["session_ns"]) if hi is None else hi
    d.mkdir(parents=True)
    with (d / f"daily_{scenario}.csv").open("w", newline="", encoding="utf-8") as stream:
        w = csv.writer(stream)
        w.writerow(COLUMNS)
        for k in range(lo, hi):
            w.writerow(["nan" if isinstance(rows[c][k], float) and not math.isfinite(rows[c][k]) else rows[c][k]
                        for c in COLUMNS])
    scen = {"scenario": scenario, "primary": True, "net_sharpe": net_sharpe, "gross_sharpe": None, "hac_t": 1.0,
            "ann_mean": 0.01, "ann_vol": 0.02, "max_drawdown": 0.01, "costs": {}, "calendar_year_returns": [],
            "construction": {"v5": {"mean_held_share": 0.9}}, "daily_turnover_gmv": {"mean": 0.06, "p95": 0.07}}
    summary = {"rule": rule, "status": "complete", "primary_scenario": scenario, "scenarios": [scen],
               "composition_weights_sha256": weights_sha, "role_sha256": role_sha}
    (d / "summary.json").write_text(json.dumps(summary), encoding="utf-8")
    return d


def history_rows(first: str, count: int, seed: int) -> dict:
    """Synthetic weekday rows beginning at ``first`` (a history era): the tiny_rows layout with its own nets."""
    rng = np.random.default_rng(seed)
    days, day = [], dt.date.fromisoformat(first)
    while len(days) < count:
        if day.weekday() < 5:
            days.append(day)
        day += dt.timedelta(days=1)
    rows = tiny_rows()
    out = {c: (list(v[:count]) if isinstance(v, list) else np.asarray(v)[:count].copy()) for c, v in rows.items()}
    out["session_ns"] = np.array([TW.session_ns(d) for d in days], dtype=np.int64)
    nets = 0.0004 + 0.01 * rng.normal(size=count)
    out["net_return"] = np.where(out["return_observation"] == 1, nets, 0.0)
    return out


def run_json(tmp: Path, name: str, argv: list[str]) -> list[dict]:
    out = tmp / f"{name}.json"
    assert NS.main(argv + ["--json", str(out)]) == 0
    rows = json.loads(out.read_text(encoding="utf-8"))
    for r in rows:
        r.pop("nav_summ_run")
    return rows


# ------------------------------------------------------------------ one era
def test_pooled_summary_over_one_era_equals_single(tmp_path, capsys):
    rows = tiny_rows()
    single = write_dir(tmp_path / "N-E3", rows)
    ref = write_dir(tmp_path / "ref", dict(rows, net_return=rows["net_return"] * 0.5 + 1e-5), role_sha="cd" * 32)
    w = tmp_path / "w.json"
    w.write_text(json.dumps({"schema": "atx.dsl-composition-weights/v1", "weights": {},
                             "provenance": {"rule": "ew-theme-v1", "weighted_standalone_turnover": 0.04}}))
    common = ["--reference", str(ref), "--weights", str(w), "--draws", "200", "--year-table", "--psr"]
    a = run_json(tmp_path, "single", [str(single), *common])
    b = run_json(tmp_path, "pool", ["--pool", str(single), "--pool-ids", "E3", *common])
    assert len(a) == len(b) == 1
    pooled = b[0]
    block = pooled.pop("pool")
    assert pooled.pop("dir") == "pool:" + str(single).replace("\\", "/") and a[0].pop("dir") == str(single)
    assert pooled == a[0]                                            # pooled == single, apart from dir and pool
    assert block["ids"] == ["E3"] and block["segments"] == [0] and block["eras"][0]["role_sha256"] == "ab" * 32
    era = dict(block["eras"][0])
    assert era.pop("id") == "E3" and era.pop("role_sha256") == "ab" * 32 and era.pop("dir") == str(single)
    assert era == {k: v for k, v in a[0].items() if k not in ("paired", "deflated", "psr")}
    text = capsys.readouterr().out
    assert "pool E3 (segments [0]): 1 era(s) in date order" in text
    # the ledger: a one-era pool is the era's own line (same trial_id, same bytes)
    la, lb = tmp_path / "a.jsonl", tmp_path / "b.jsonl"
    assert NS.main([str(single), "--ledger", str(la)]) == 0
    assert NS.main(["--pool", str(single), "--pool-ids", "E3", "--ledger", str(lb)]) == 0
    assert la.read_bytes() == lb.read_bytes() and len(la.read_text().splitlines()) == 1
    assert RL.ledger_n(la, "x", pool_dirs=[single]) == 1                 # matched by the (pooled == era) trial_id


def test_a_one_era_pool_is_refused_when_its_window_straddles_the_train_begin(tmp_path):
    rows = history_rows("2019-10-01", 120, 5)                          # 2019-10 .. 2020-03
    d = write_dir(tmp_path / "N", rows)
    assert NS.main([str(d)]) == 0                                      # the single path reads it as before
    with pytest.raises(SystemExit, match="straddles the TRAIN begin"):
        NS.main(["--pool", str(d)])


# ------------------------------------------------------------------ two eras
@pytest.fixture()
def two_eras(tmp_path):
    split = 136
    rows = tiny_rows(split)
    single = write_dir(tmp_path / "N-all", rows)
    e1 = write_dir(tmp_path / "N-E1", rows, 0, split, role_sha="e1" * 32)
    e2 = write_dir(tmp_path / "N-E2", rows, split, None, role_sha="e2" * 32)
    return {"rows": rows, "split": split, "single": single, "e1": e1, "e2": e2, "tmp": tmp_path}


def test_two_era_pool_equals_the_single_csv_on_every_return_and_turnover_statistic(two_eras):
    t = two_eras
    tmp, split, rows = t["tmp"], t["split"], t["rows"]
    single = run_json(tmp, "single", [str(t["single"]), "--year-table"])[0]
    pooled = run_json(tmp, "pool", ["--pool", str(t["e1"]), str(t["e2"]), "--pool-ids", "E1,E2", "--year-table"])[0]
    for key in SAME_KEYS:
        assert pooled[key] == single[key], key
    assert pooled["year_table"] == single["year_table"]
    daily = NS.load_daily(t["single"], SCEN)
    ret = NS.return_mask(daily)
    assert pooled["net_sharpe"] == pytest.approx(NS.sharpe(daily["net_return"][ret]), rel=1e-12)
    assert pooled["gross_sharpe"] == pytest.approx(NS.sharpe(daily["gross_return"][ret]), rel=1e-12)
    assert pooled["hac_t"] is None and pooled["held_share"] is None and single["held_share"] == 0.9
    # the ramp is per era: 63 rows dropped at the start of each era
    gl = np.asarray(daily["gross_leverage"])
    post = np.r_[gl[63:split], gl[split + 63:]]
    assert pooled["post_ramp_rows"] == post.size == len(gl) - 126
    assert pooled["mean_gross_leverage_post_ramp"] == pytest.approx(float(post.mean()), rel=1e-15)
    assert single["post_ramp_rows"] == len(gl) - 63
    p = pooled["pool"]
    assert p["segments"] == [0, split] and p["ids"] == ["E1", "E2"]
    assert [e["return_rows"] for e in p["eras"]] == [split - 2, len(gl) - split - 2]
    assert sum(e["return_rows"] for e in p["eras"]) == pooled["return_rows"]
    assert [e["role_sha256"] for e in p["eras"]] == ["e1" * 32, "e2" * 32]


def test_pool_refuses_overlap_order_scenario_and_a_sealed_era(two_eras, tmp_path):
    t = two_eras
    rows, split = t["rows"], t["split"]
    with pytest.raises(SystemExit, match="date order"):
        NS.main(["--pool", str(t["e2"]), str(t["e1"])])
    late = write_dir(tmp_path / "late", rows, split - 10, None)
    with pytest.raises(SystemExit, match="overlaps era E1"):
        NS.main(["--pool", str(t["e1"]), str(late)])
    other = write_dir(tmp_path / "other", rows, split, None, scenario="modeled-other")
    with pytest.raises(SystemExit, match="one scenario and one rule"):
        NS.main(["--pool", str(t["e1"]), str(other)])
    rule = write_dir(tmp_path / "rule", rows, split, None, rule="other-rule")
    with pytest.raises(SystemExit, match="one scenario and one rule"):
        NS.main(["--pool", str(t["e1"]), str(rule)])
    with pytest.raises(SystemExit, match="--pool-ids"):
        NS.main(["--pool", str(t["e1"]), str(t["e2"]), "--pool-ids", "E1"])
    with pytest.raises(SystemExit, match="must match"):
        NS.main(["--pool", str(t["e1"]), "--pool-ids", "E/1"])
    # a synthetic era dated at the seal: refused on its session column before any statistic
    sealed = history_rows("2023-12-01", 40, 3)
    d = write_dir(tmp_path / "sealed", sealed)
    with pytest.raises(SystemExit, match="seal"):
        NS.main(["--pool", str(t["e1"]), str(d)])


def test_pooled_paired_block_against_a_pooled_reference_or_a_note(two_eras, tmp_path, capsys):
    t = two_eras
    rows, split = t["rows"], t["split"]
    ref_rows = dict(rows, net_return=rows["net_return"] * 0.7 + 2e-5)
    r1 = write_dir(tmp_path / "R-E1", ref_rows, 0, split)
    r2 = write_dir(tmp_path / "R-E2", ref_rows, split, None)
    pooled = run_json(tmp_path, "p", ["--pool", str(t["e1"]), str(t["e2"]), "--pool-reference", str(r1), str(r2),
                                      "--draws", "200"])[0]
    a, b = NS.align(NS.net_series(NS.load_daily(t["single"], SCEN)),
                    NS.net_series(NS.load_daily(write_dir(tmp_path / "R-all", ref_rows), SCEN)))
    assert pooled["paired"]["sessions"] == a.size and pooled["paired"]["dsr"] == pytest.approx(
        NS.sharpe(a) - NS.sharpe(b), rel=1e-12)
    hist = write_dir(tmp_path / "H", history_rows("2014-01-02", 80, 1))   # no session in common with the eras
    pooled = run_json(tmp_path, "q", ["--pool", str(t["e1"]), str(t["e2"]), "--reference", str(hist)])[0]
    assert "paired" not in pooled and pooled["paired_note"].startswith("paired skipped: 0 session(s)")
    assert "paired skipped" in capsys.readouterr().out


# ------------------------------------------------------------------ the ledger
def test_ledger_lines_per_era_plus_one_pooled_line_add_one_trial(two_eras, tmp_path, capsys):
    t = two_eras
    ledger = tmp_path / "trials.jsonl"
    other = write_dir(tmp_path / "cell0", t["rows"])
    assert NS.main([str(other), "--ledger", str(ledger)]) == 0         # one TRAIN cell ledgered first
    before = BI.ledger_read(ledger)
    n0 = BI.ledger_n(before, False)
    pool_dirs = [t["e1"], t["e2"]]
    label = "pool:" + ",".join(str(d).replace("\\", "/") for d in pool_dirs)
    assert RL.ledger_n(ledger, label, pool_dirs=pool_dirs) == n0 == 2  # plan time: + 1 for the pooled cell
    argv = ["--pool", *map(str, pool_dirs), "--pool-ids", "E1,E2", "--ledger", str(ledger), "--protocol", "v8",
            "--origin", "prior"]
    assert NS.main(argv) == 0
    recs = BI.ledger_read(ledger)
    new = recs[len(before):]
    assert [BI.is_era_line(r) for r in new] == [True, True, False] and BI.is_pool_line(new[2])
    pooled = new[2]
    shas = [BI.sha256_file(d / f"daily_{SCEN}.csv") for d in pool_dirs]
    assert pooled["trial_id"] == BI.pooled_trial_id("construction", shas) == new[0]["era_of"] == new[1]["era_of"]
    assert pooled["window"]["label"] == "POOL" and [r["window"]["label"] for r in new[:2]] == ["ERA E1", "ERA E2"]
    assert [e["trial_id"] for e in pooled["eras"]] == [new[0]["trial_id"], new[1]["trial_id"]]
    assert [e["role_sha256"] for e in pooled["eras"]] == ["e1" * 32, "e2" * 32]
    assert pooled["series"]["sha256"] == BI.era_pool().pooled_sha256(shas) and pooled["origin"] == "prior"
    daily = NS.load_daily(t["single"], SCEN)
    assert pooled["s2_net_sr"] == pytest.approx(NS.sharpe(daily["net_return"][NS.return_mask(daily)]), rel=1e-12)
    assert BI.trial_counts(recs)[len(before):] == [0, 0, 1]
    assert BI.ledger_n(recs, True) == n0                              # N + 1 in total, not + 3
    assert BI.history_read_lines(recs) == [] and "; history reads 0" in BI.appendix_a_v8(recs)   # eras inside TRAIN
    cell0 = str(other).replace("\\", "/")
    assert RL.cells(ledger) == [cell0]                                 # era and pooled lines are no grid cells
    assert RL.ledger_n(ledger, label, pool_dirs=pool_dirs) == n0      # now ledgered: matched by the pooled trial_id
    lines = BI.appendix_a(recs, str(ledger))
    assert any("era shard line(s): 2, adding no trial" in line and "1 pooled line(s)" in line for line in lines)
    assert not any("adding no trial:" in line for line in lines)      # the v8 zero-trial line counts no era line
    v = BI.dsr_variance(recs, BI.window_id())                          # era and pooled lines stay out of V[SR]
    assert (v["cells"], v["legacy_cells"]) == (0, 1)                   # (the pooled line carries the window id)
    assert BI.ledger_net_series(recs, lambda r: {})[0] == [cell0]
    capsys.readouterr()
    assert NS.main(argv) == 0                                          # an identity re-run adds nothing
    assert any("appended 0, skipped 3" in line for line in capsys.readouterr().out.splitlines())
    assert len(BI.ledger_read(ledger)) == len(recs)


def test_a_defect_or_rerun_names_the_pooled_trial_line_only(two_eras, tmp_path):
    t = two_eras
    ledger = tmp_path / "d.jsonl"
    assert NS.main(["--pool", str(t["e1"]), str(t["e2"]), "--ledger", str(ledger), "--ledger-defect", "bad fill"]) == 0
    recs = BI.ledger_read(ledger)
    assert ["defect" in r for r in recs] == [False, False, True] and BI.trial_counts(recs) == [0, 0, 0]
    assert BI.excluded_lines(recs) == [recs[2]]
    with pytest.raises(ValueError, match="go together"):
        BI.ledger_pool_records("construction", [{}, {}], {}, None, rerun_of="x")


def test_history_eras_are_ledgered_by_the_era_rule_and_a_single_history_cell_is_still_refused(tmp_path):
    e1 = write_dir(tmp_path / "H-E1", history_rows("2014-01-02", 90, 1), role_sha="e1" * 32)
    e2 = write_dir(tmp_path / "H-E2", history_rows("2017-01-03", 90, 2), role_sha="e2" * 32)
    ledger = tmp_path / "t.jsonl"
    with pytest.raises(SystemExit, match="only TRAIN series"):
        NS.main([str(e1), "--ledger", str(ledger)])
    assert NS.main(["--pool", str(e1), str(e2), "--ledger", str(ledger)]) == 0
    recs = BI.ledger_read(ledger)
    assert [r["window"]["label"] for r in recs] == ["ERA E1", "ERA E2", "POOL"]
    assert recs[2]["window"]["first_session"] == "2014-01-06"          # the first return row (a flat, a deployment)
    # Ruling E-41: a pooled history read adds 0 to N and counts once as a history read (its pooled line)
    assert BI.trial_counts(recs) == [0, 0, 0] and BI.history_read_lines(recs) == [recs[2]]
    assert BI.ledger_n(recs, True) == 0 and "; history reads 1" in BI.appendix_a_v8(recs)
    rows = run_json(tmp_path, "dsr", ["--pool", str(e1), str(e2), "--dsr-ledger", str(ledger)])
    q = rows[0]["deflated_ledger"]                                     # its own DSR: N without + 1
    assert q["n"] == BI.ledger_n(recs, True) == 0 and q["n_rule"].endswith("(a history read adds no trial, Ruling E-41)")
    with pytest.raises(ValueError, match="TRAIN end"):
        BI.window_of([BI.research_window().TRAIN_END_NS], era="E9")


# ------------------------------------------------------------------ review P-1: a history read on one era
def test_a_one_era_history_read_names_the_train_cell_it_re_reads(tmp_path, capsys):
    """Every history-read line carries era_of: a one-era history pool names the TRAIN cell it re-reads (--era-of, a
    ledgered cell line of its kind); its line is an era line (adds 0) that no reader takes for a TRAIN cell."""
    e1 = write_dir(tmp_path / "H-E1", history_rows("2014-01-02", 90, 1), role_sha="e1" * 32)
    e2 = write_dir(tmp_path / "H-E2", history_rows("2017-01-03", 90, 2), role_sha="e2" * 32)
    cell = write_dir(tmp_path / "cell0", tiny_rows())
    one = tmp_path / "one.jsonl"
    assert NS.main([str(cell), "--ledger", str(one)]) == 0             # the TRAIN cell, ledgered first
    (train_line,) = BI.ledger_read(one)
    hist = ["--pool", str(e1), "--pool-ids", "E1", "--ledger", str(one)]
    with pytest.raises(SystemExit, match=r"re-reads a TRAIN cell: .*--era-of TRIAL_ID"):
        NS.main(hist)                                                  # without the TRAIN cell: refused
    for bad, needle in (("abc", "got 'abc'"), ("0" * 16, "is not the trial_id of a ledgered construction cell")):
        with pytest.raises(SystemExit, match=needle):
            NS.main([*hist, "--era-of", bad])
    assert BI.ledger_read(one) == [train_line]                         # every refusal appends nothing
    assert NS.main([*hist, "--era-of", train_line["trial_id"]]) == 0
    recs = BI.ledger_read(one)
    line = recs[1]
    assert line["window"]["label"] == "ERA E1" and line["era"] == {"id": "E1", "role_sha256": "e1" * 32}
    assert line["era_of"] == train_line["trial_id"] and BI.is_era_line(line) and BI.is_history_line(line)
    assert BI.trial_counts(recs) == [1, 0] and BI.ledger_n(recs, True) == 1
    assert BI.history_read_lines(recs) == [line]                       # Ruling E-41: one history read
    cell0 = str(cell).replace("\\", "/")
    assert RL.cells(one) == [cell0] and BI.ledger_net_series(recs, lambda r: {})[0] == [cell0]
    assert BI.dsr_variance(recs, BI.window_id())["legacy_cells"] == 1
    # --era-of is for a one-era history pool only
    with pytest.raises(SystemExit, match="a pool of two or more eras is its own trial"):
        NS.main(["--pool", str(e1), str(e2), "--ledger", str(tmp_path / "two.jsonl"), "--era-of",
                 train_line["trial_id"]])
    inside = write_dir(tmp_path / "N-E3", tiny_rows())
    with pytest.raises(SystemExit, match="scored inside TRAIN"):
        NS.main(["--pool", str(inside), "--pool-ids", "E3", "--ledger", str(tmp_path / "in.jsonl"), "--era-of",
                 train_line["trial_id"]])
    capsys.readouterr()
    with pytest.raises(SystemExit) as e:
        NS.main([str(cell), "--era-of", train_line["trial_id"]])
    assert e.value.code == 2 and "--era-of needs --pool and --ledger" in capsys.readouterr().err


@pytest.fixture()
def p1_ledger(tmp_path):
    """A v8 ledger holding two TRAIN cells and one history read on one era written before the fix (P-1's line: label
    ERA, an era block and window_id, no era_of), appended as the H-1 writer appended it."""
    rows = tiny_rows()
    cells = [write_dir(tmp_path / f"cell{k}", dict(rows, net_return=rows["net_return"] * (1.0 + k) + 1e-4 * k))
             for k in range(2)]
    ledger = tmp_path / "trials.jsonl"
    for d in cells:
        assert NS.main([str(d), "--ledger", str(ledger), "--protocol", "v8", "--origin", "prior"]) == 0
    hist = write_dir(tmp_path / "H-E1", history_rows("2014-01-02", 90, 1), role_sha="e1" * 32)
    daily = hist / f"daily_{SCEN}.csv"
    nets = NS.net_series(NS.load_daily(hist, SCEN))
    line = BI.ledger_record("construction", str(hist), hist / "summary.json", daily, SCEN, nets, 0.9,
                            era={"id": "E1", "role_sha256": "e1" * 32}, origin="prior",
                            research_window_id=BI.window_id())
    BI.ledger_append(ledger, [line], chain=True)
    recs = BI.ledger_read(ledger)
    assert len(recs) == 3 and recs[2]["window"]["label"] == "ERA E1" and "era" in recs[2]
    assert not BI.is_era_line(recs[2]) and not BI.is_pool_line(recs[2]) and BI.is_history_line(recs[2])
    assert recs[2]["window_id"] == BI.window_id() == recs[0]["window_id"]
    return {"ledger": ledger, "records": recs, "cells": [str(d).replace("\\", "/") for d in cells]}


def test_p1_dsr_variance_leaves_a_history_line_without_era_of_out(p1_ledger):
    recs = p1_ledger["records"]
    v = BI.dsr_variance(recs, BI.window_id())
    srs = [r["s2_net_sr"] / math.sqrt(BI.ANNUAL) for r in recs[:2]]
    assert v["cells"] == 2 and v["variance_sr"] == pytest.approx(float(np.var(srs, ddof=1)), rel=1e-15)
    assert v["legacy_cells"] == 0
    assert BI.dsr_variance(recs[:2], BI.window_id())["variance_sr"] == v["variance_sr"]   # as if it were absent


def test_e41_appendix_a_counts_the_history_read_apart_and_n_is_unchanged(p1_ledger):
    """Ruling E-41 on the P-1 fixture ledger: the history read (written before every such line carried era_of) adds 0
    to the construction N and 1 to the history reads printed beside the validation reads."""
    recs = p1_ledger["records"]
    assert BI.trial_counts(recs) == [1, 1, 0] and BI.history_read_lines(recs) == [recs[2]]
    assert BI.ledger_n(recs, True) == BI.ledger_n(recs[:2], True) == 2
    block = BI.appendix_a_v8(recs)
    assert block.startswith("TRAIN construction cells 2; ")
    assert "validation reads before v8: 2 (2023-2024); history reads 1" in block
    assert "; history reads 0" in BI.appendix_a_v8(recs[:2])
    text = BI.appendix_a(recs, "t")
    assert text[0] == "Appendix A (trial ledger t): 2 trials in 3 ledger lines" and not any("ERA" in x for x in text)
    assert text[-1].startswith("   history read(s): 1, adding no trial (Ruling E-41")


def test_p1_research_ledger_cells_skips_a_history_line_without_era_of(p1_ledger):
    assert RL.cells(p1_ledger["ledger"]) == p1_ledger["cells"]       # never a positional cell of a later summ


def test_p1_ledger_net_series_skips_a_history_line_without_era_of(p1_ledger):
    names, series = BI.ledger_net_series(p1_ledger["records"],
                                         lambda r: NS.net_series(NS.load_daily_csv(Path(r["series"]["path"]))))
    assert names == p1_ledger["cells"] and len(series) == 2
