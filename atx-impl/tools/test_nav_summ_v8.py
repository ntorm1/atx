"""Validation kit tests for the moved nav_summ.py / backtest_integrity.py (platform v8 V-1).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_nav_summ_v8.py

Synthetic data only, except test_legacy_n37_numbers_reproduced: it runs only when ATX_EQUITY_ROOT names a
``build-equity`` directory (root runs it) and then reads the v7.1 2020-2022 NAV cells listed in
mega-nav-v71-summ-n37.json. Every window date comes from research_window.py (task W0-1).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys

import numpy as np
import pytest

import backtest_integrity as BI
import nav_summ as NS
from test_nav_summ import SCEN, T0, DAY, write_nav

TOOLS = Path(__file__).resolve().parent
SHIM = TOOLS.parents[1] / ".superpowers" / "sdd" / "mega-alpha-20260926" / "studies" / "nav_summ.py"
PY = sys.executable


def cell(tmp_path: Path, name: str, nets, **kw) -> Path:
    return write_nav(tmp_path / name, list(nets), **kw)


def noise_cells(tmp_path: Path, k: int, t: int = 300, seed: int = 0, prefix: str = "cell") -> list[Path]:
    rng = np.random.default_rng(seed)
    return [cell(tmp_path, f"{prefix}{i}", 0.0003 * (i + 1) + 0.01 * rng.normal(size=t)) for i in range(k)]


def record(d: Path, sr: float, **kw) -> dict:
    return BI.ledger_record("construction", str(d), d / "summary.json", d / f"daily_{SCEN}.csv", SCEN,
                            NS.net_series(NS.load_daily(d, SCEN)), sr, **kw)


def canonical(doc) -> str:
    return json.dumps(doc, indent=2, sort_keys=True) + "\n"


def lane_a_protocol_line(ledger: Path, window_sha: str) -> dict:
    """A protocol line exactly as `research_cycle.py ledger-protocol` (lane A, research_ledger.py) writes it: no cell,
    count 0, no prev_sha256, compact sorted-key JSON."""
    wid, date = BI.window_id(), "2026-09-29"
    rec = {"schema": BI.LEDGER_SCHEMA, "kind": "protocol", "count": 0, "window_id": wid,
           "owner_ruling": "expand TRAIN to include 2023; keep 2024+ hidden and out of sample", "date": date,
           "research_window_sha256": window_sha}
    ident = json.dumps(["protocol", wid, window_sha, date], separators=(",", ":"))
    rec["trial_id"] = hashlib.sha256(ident.encode()).hexdigest()[:16]
    with ledger.open("a", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(rec, sort_keys=True, separators=(",", ":")) + "\n")
    return rec


# ------------------------------------------------------------------ legacy reproduction (root only)
def test_legacy_n37_numbers_reproduced(tmp_path, monkeypatch, capsys):
    """The moved scripts, run with the n37 argv on the v7.1 cells, give mega-nav-v71-summ-n37.json and the PBO JSON
    byte for byte except nav_summ_run (script path, script SHA-256, git head) and the ledger path of each row."""
    root = os.environ.get("ATX_EQUITY_ROOT")
    if not root or Path(root).name != "build-equity" or not Path(root).is_dir():
        pytest.skip("set ATX_EQUITY_ROOT to a build-equity directory (root runs this test on the v7.1 cells)")
    equity = Path(root).resolve()
    summ = equity / "mega-nav-v71-summ-n37.json"
    pbo = equity / "mega-nav-v71-pbo-n37.json"
    want_rows = json.loads(summ.read_text(encoding="utf-8"))
    assert canonical(want_rows) == summ.read_text(encoding="utf-8")    # the file is nav_summ's canonical dump
    argv = list(want_rows[0]["nav_summ_run"]["argv"])
    # the ledger as the n37 run saw it: its appended lines (if any) removed, every other line verbatim
    added = {r["ledger"]["trial_id"] for r in want_rows if r.get("ledger", {}).get("appended")}
    ledger_arg = argv[argv.index("--ledger") + 1]
    lines = (equity.parent / ledger_arg).read_text(encoding="utf-8").splitlines()
    ledger = tmp_path / "trials.jsonl"
    ledger.write_text("".join(line + "\n" for line in lines
                              if line.strip() and json.loads(line).get("trial_id") not in added), encoding="utf-8")
    swap = {"--ledger": str(ledger), "--ledger-n": str(ledger), "--json": str(tmp_path / "summ.json"),
            "--pbo-json": str(tmp_path / "pbo.json")}
    for flag, value in swap.items():
        if flag in argv:
            argv[argv.index(flag) + 1] = value
    monkeypatch.chdir(equity.parent)
    assert NS.main(argv) == 0
    capsys.readouterr()
    got_rows = json.loads((tmp_path / "summ.json").read_text(encoding="utf-8"))
    assert len(got_rows) == len(want_rows) == 37
    for got, want in zip(got_rows, want_rows):
        got.pop("nav_summ_run"), want.pop("nav_summ_run")
        if "ledger" in got:
            got["ledger"]["path"] = want["ledger"]["path"]
    assert canonical(got_rows) == canonical(want_rows)
    if "--pbo-json" in argv and pbo.exists():
        got_pbo = json.loads((tmp_path / "pbo.json").read_text(encoding="utf-8"))
        want_pbo = json.loads(pbo.read_text(encoding="utf-8"))
        got_pbo.pop("nav_summ_run"), want_pbo.pop("nav_summ_run")
        assert canonical(got_pbo) == canonical(want_pbo)


# ------------------------------------------------------------------ year table
def test_year_table(tmp_path, capsys):
    n = 900                                   # rows 2020-01-02 .. 2022-06-20 (calendar-day sessions)
    nets = [0.001 * (1 + j % 3) - 0.0015 * (j % 2) for j in range(n)]
    t = n + 2
    tau = [0.01 + 1e-5 * k for k in range(t)]
    d = cell(tmp_path, "c", nets, tau=tau)
    rows = NS.year_table(NS.load_daily(d, SCEN))
    # expected, row by row (row 0: no return; row 1: deployment, executed, no return row; rows 2..: return rows)
    exp: dict = {}
    traded = [0.0, 5e8] + [1e7] * (t - 2)
    cost = [0.0, 1e5] + [2e3] * (t - 2)
    for k in range(t):
        y = (dt.date(1970, 1, 1) + dt.timedelta(days=(T0 + k * DAY) // DAY)).year
        e = exp.setdefault(y, {"x": [], "tau": [], "traded": 0.0, "cost": 0.0})
        if k >= 1:
            e["traded"] += traded[k]
            e["cost"] += cost[k]
        if k >= 2:
            e["x"].append(nets[k - 2])
            e["tau"].append(tau[k])
    assert [r["year"] for r in rows] == [2020, 2021, 2022]
    for r in rows:
        e = exp[r["year"]]
        x = np.array(e["x"])
        assert r["return_rows"] == len(e["x"])
        assert r["net_return"] == pytest.approx(float(np.prod(1 + x) - 1), rel=1e-12)
        assert r["net_sharpe"] == pytest.approx(x.mean() / x.std(ddof=1) * math.sqrt(252), rel=1e-12)
        assert r["ann_vol"] == pytest.approx(x.std(ddof=1) * math.sqrt(252), rel=1e-12)
        assert r["tau_gmv_mean"] == pytest.approx(float(np.mean(e["tau"])), rel=1e-12)
        assert r["cost_bps_traded"] == pytest.approx(1e4 * e["cost"] / e["traded"], rel=1e-12)
    assert sum(r["return_rows"] for r in rows) == n
    # CLI: --year-table prints it and --json carries it; without the option neither appears (v7 output unchanged)
    assert NS.main([str(d), "--year-table", "--json", str(tmp_path / "y.json")]) == 0
    out = capsys.readouterr().out
    assert "year table (return rows, compounded net return" in out and "  2021 rows  365 net " in out
    assert json.loads((tmp_path / "y.json").read_text(encoding="utf-8"))[0]["year_table"] == rows
    assert NS.main([str(d), "--json", str(tmp_path / "n.json")]) == 0
    assert "year table" not in capsys.readouterr().out
    assert "year_table" not in json.loads((tmp_path / "n.json").read_text(encoding="utf-8"))[0]


# ------------------------------------------------------------------ bundle verdict
def test_bundle_verdict(tmp_path, capsys):
    rng = np.random.default_rng(11)
    t = 800
    base = 0.0002 + 0.01 * rng.normal(size=t)
    better = base + 0.001 + 0.002 * rng.normal(size=t)
    b, f = cell(tmp_path, "b0c", base), cell(tmp_path, "v8f", better)
    args = argparse.Namespace(scenario=None, draws=199, block=21, seed=NS.V8_SEED, bundle_alpha=NS.BUNDLE_ALPHA)
    res = NS.bundle(b, f, args)
    p, v = res["paired"], res["verdict"]
    assert p["dsr"] > 0 and p["lw"]["p_one_sided"] is not None and p["lw"]["p_one_sided"] < 0.10
    assert v["dsr_positive"] and v["pass"] and v["alpha"] == 0.10
    assert p["draws"] == 199 and p["block"] == 21 and p["seed"] == 20260929
    # per-year dSR on the common sessions, and each cell's year table
    nets_f, nets_b = NS.net_series(NS.load_daily(f, SCEN)), NS.net_series(NS.load_daily(b, SCEN))
    assert [y["year"] for y in res["years"]] == [2020, 2021, 2022]
    for y in res["years"]:
        s = [k for k in nets_f if BI.session_date(k).year == y["year"]]
        sf = NS.sharpe(np.array([nets_f[k] for k in s]))
        sb = NS.sharpe(np.array([nets_b[k] for k in s]))
        assert y["sessions"] == len(s) and y["dsr"] == pytest.approx(sf - sb, rel=1e-12)
    assert [r["year"] for r in res["year_table"]["final"]] == [2020, 2021, 2022]
    # the reverse bundle fails: dSR < 0
    rev = NS.bundle(f, b, args)
    assert rev["paired"]["dsr"] < 0 and not rev["verdict"]["dsr_positive"] and not rev["verdict"]["pass"]
    # one-sided p from the same resamples is at most the two-sided p when dSR > 0
    assert p["lw"]["p_one_sided"] <= p["lw"]["p_value"]
    # a tie: identical cells are never a pass (dSR 0)
    assert not NS.bundle(b, b, args)["verdict"]["pass"]
    # CLI: v8 defaults (seed 20260929), --bundle-json
    out_json = tmp_path / "bundle.json"
    assert NS.main(["--bundle", str(b), str(f), "--protocol", "v8", "--draws", "199", "--bundle-json",
                    str(out_json)]) == 0
    out = capsys.readouterr().out
    assert "== bundle " in out and "verdict PASS" in out
    doc = json.loads(out_json.read_text(encoding="utf-8"))
    assert doc["protocol"] == "v8" and doc["paired"]["seed"] == 20260929 and doc["verdict"]["pass"] is True
    assert doc["paired"]["dsr"] == pytest.approx(p["dsr"], rel=1e-15)


# ------------------------------------------------------------------ ledger: origin, window_id, chain
def test_origin_class_in_ledger_line(tmp_path, capsys, monkeypatch):
    monkeypatch.chdir(tmp_path)
    dirs = [str(Path(d).relative_to(tmp_path)) for d in noise_cells(tmp_path, 3)]
    ledger = "trials.jsonl"
    # v7 (no --protocol): the line layout is the v7 one (no origin, window_id or prev_sha256)
    assert NS.main([dirs[0], "--ledger", ledger]) == 0
    v7 = BI.ledger_read(Path(ledger))[0]
    assert not {"origin", "window_id", "prev_sha256", "defect", "rerun_of"} & set(v7)
    # v8: origin (K5) and window_id in every appended line, chained to the previous line
    assert NS.main([dirs[1], dirs[2], "--ledger", ledger, "--protocol", "v8", "--origin", "grid"]) == 0
    rows = BI.ledger_read(Path(ledger))
    assert [r.get("origin") for r in rows] == [None, "grid", "grid"]
    assert [r.get("window_id") for r in rows] == [None, BI.window_id(), BI.window_id()]
    text = Path(ledger).read_text(encoding="utf-8").splitlines()
    assert rows[1]["prev_sha256"] == BI.chain_head(text[:1]) and rows[2]["prev_sha256"] == BI.line_sha256(text[1])
    # trial_id rule unchanged: the v8 fields never enter it
    for d, r in zip(dirs, rows):
        assert r["trial_id"] == BI.trial_id("construction", BI.sha256_file(Path(d) / f"daily_{SCEN}.csv"))
    capsys.readouterr()
    # v8 without an origin, an unknown origin, and --origin without --ledger are usage errors
    with pytest.raises(SystemExit):
        NS.main([dirs[0], "--ledger", "x.jsonl", "--protocol", "v8"])
    with pytest.raises(SystemExit):
        NS.main([dirs[0], "--ledger", "x.jsonl", "--origin", "lucky"])
    with pytest.raises(SystemExit):
        NS.main([dirs[0], "--origin", "prior"])
    with pytest.raises(ValueError, match="origin"):
        record(Path(dirs[0]), 1.0, origin="lucky")
    assert not Path("x.jsonl").exists()
    # every origin class is accepted
    for o, d in zip(BI.ORIGINS, noise_cells(tmp_path, 3, seed=5, prefix="o")):
        assert record(d, 1.0, origin=o)["origin"] == o


# ------------------------------------------------------------------ DSR variance from re-run cells (OD-4)
def test_variance_from_rerun_cells(tmp_path, capsys):
    wid = BI.window_id()
    cells = noise_cells(tmp_path, 8)
    ledger = tmp_path / "trials.jsonl"
    legacy = [record(cells[i], sr) for i, sr in enumerate((0.5, 1.0, 1.5))]
    BI.ledger_append(ledger, legacy)
    lines = [record(cells[3], 0.6, research_window_id=wid, rerun_of=legacy[0]["trial_id"], rerun_basis="window"),
             record(cells[4], 1.1, research_window_id=wid, rerun_of=legacy[1]["trial_id"], rerun_basis="window"),
             record(cells[5], 0.9, research_window_id=wid, origin="prior"),
             record(cells[6], 1.3, research_window_id=wid, origin="grid"),
             record(cells[7], 5.0, research_window_id=wid, origin="mined", defect="cost model misread")]
    BI.ledger_append(ledger, lines, chain=True)
    records = BI.ledger_read(ledger)
    assert BI.trial_counts(records) == [1, 1, 1, 0, 0, 1, 1, 0]
    v = BI.dsr_variance(records, wid)
    root = math.sqrt(252)
    assert v["n"] == 5 and v["cells"] == 4 and v["legacy_cells"] == 3 and v["window_id"] == wid
    assert v["variance_sr"] == pytest.approx(float(np.var(np.array([0.6, 1.1, 0.9, 1.3]) / root, ddof=1)), rel=1e-12)
    assert v["legacy_variance_sr"] == pytest.approx(float(np.var(np.array([0.5, 1.0, 1.5]) / root, ddof=1)),
                                                    rel=1e-12)
    # nav_summ --dsr-ledger: N = the construction trials (+1 for a cell not in the ledger), V[SR] from this window
    new = cell(tmp_path, "new", 0.0008 + 0.01 * np.random.default_rng(9).normal(size=300))
    out_json = tmp_path / "d.json"
    assert NS.main([str(new), str(cells[5]), "--dsr-ledger", str(ledger), "--json", str(out_json)]) == 0
    out = capsys.readouterr().out
    assert f"   ledger DSR (N=6, {wid}): DSR " in out and f"   ledger DSR (N=5, {wid}): DSR " in out
    rows = json.loads(out_json.read_text(encoding="utf-8"))
    q = rows[0]["deflated_ledger"]
    assert q["variance_sr"] == pytest.approx(v["variance_sr"], rel=1e-15) and q["cells"] == 4
    m = rows[0]["net_moments"]
    sr0 = NS.expected_max_sr(v["variance_sr"], 6)
    assert q["dsr"] == pytest.approx(NS.deflated_sharpe(m["sr_daily"], m["sessions"], m["skew"], m["kurtosis"], sr0),
                                     rel=1e-12)
    assert q["legacy_dsr"] is not None and q["legacy_cells"] == 3        # reported beside it
    # with fewer than two cells on the window there is no current variance (never a silent fallback)
    assert BI.dsr_variance(records[:4], wid)["variance_sr"] is None


def test_defect_rule_appendix_a(tmp_path):
    """Appendix A rule 7: an invalid cell is logged and leaves N; a blind re-run replaces it (one trial: the replaced
    cell stays counted, the re-run adds 0, review C-5); a re-run decided because the returns looked wrong keeps both
    as trials. A cell found invalid after it was ledgered is marked by a defect line (review C-3)."""
    wid = BI.window_id()
    c = noise_cells(tmp_path, 6)
    bad = record(c[0], 0.4, research_window_id=wid, origin="prior", defect="stale fields manifest")
    blind = record(c[1], 0.7, research_window_id=wid, origin="prior", rerun_of=bad["trial_id"], rerun_basis="blind")
    looked = record(c[2], 0.2, research_window_id=wid, origin="grid", defect="fills priced at the wrong close")
    after = record(c[3], 0.9, research_window_id=wid, origin="grid", rerun_of=looked["trial_id"],
                   rerun_basis="returns")
    replaced = record(c[4], 0.3, research_window_id=wid, origin="mined")
    found = BI.defect_line(replaced["trial_id"], "borrow fee table misread", ruling="E-31")
    blind2 = record(c[5], 0.5, research_window_id=wid, origin="mined", rerun_of=replaced["trial_id"],
                    rerun_basis="blind")
    # review F-1: a re-run of a cell ledgered invalid at once needs the owner ruling's id, on a defect line
    rulings = [BI.defect_line(x["trial_id"], x["defect"]["reason"], ruling="E-31") for x in (bad, looked)]
    ledger = tmp_path / "t.jsonl"
    BI.ledger_append(ledger, [bad, rulings[0], blind, looked, rulings[1], after, replaced, found, blind2], chain=True)
    recs = BI.ledger_read(ledger)
    assert BI.trial_counts(recs) == [1, 0, 0, 1, 0, 1, 1, 0, 0]
    assert [r["trial_id"] for r in BI.excluded_lines(recs)] == [bad["trial_id"], replaced["trial_id"]]   # out of V
    assert bad["defect"] == {"invalid": True, "reason": "stale fields manifest"}
    text = BI.appendix_a(recs, "t.jsonl")
    assert text[0] == "Appendix A (trial ledger t.jsonl): 4 trials in 9 ledger lines"
    assert text[-1] == "   adding no trial: 5 line(s) (5 by the defect rule, 0 window re-run(s), 0 protocol line(s))"
    # the v8 block: every window date from research_window.py
    rw = BI.research_window()
    v8 = BI.appendix_a_v8(recs)
    first, last = BI.session_date(rw.TRAIN_BEGIN_NS).year, BI.session_date(rw.TRAIN_END_NS - DAY).year
    assert v8.startswith(f"TRAIN construction cells 4; admission trials this sprint 0; window {wid} ({first}-{last}); "
                         f"hidden {BI.session_date(rw.SEAL_NS).year}+ unread in this sprint")
    with pytest.raises(ValueError, match="rerun_of and rerun_basis"):
        record(c[0], 0.1, rerun_of=bad["trial_id"])
    with pytest.raises(ValueError, match="defect needs a reason"):
        record(c[0], 0.1, defect=" ")


# ------------------------------------------------------------------ protocol line (W0-3)
def test_protocol_line_is_chained_but_not_counted(tmp_path, capsys):
    cells = noise_cells(tmp_path, 3)
    ledger = tmp_path / "trials.jsonl"
    BI.ledger_append(ledger, [record(cells[0], 0.5), record(cells[1], 0.8)], chain=True)
    unchained = tmp_path / "unchained.jsonl"                 # the pre-A-3 writer (unchained) after chained lines:
    unchained.write_bytes(ledger.read_bytes())               # refused since review C-6
    lane_a_protocol_line(unchained, "ab" * 32)
    with pytest.raises(ValueError, match="an unchained line after the chained line 1"):
        BI.ledger_read(unchained)
    proto = lane_a_protocol_line(tmp_path / "proto.jsonl", "ab" * 32)
    (tmp_path / "proto.jsonl").unlink()
    BI.ledger_append(ledger, [proto], chain=True)            # lane A's writer today (research_ledger.append): chained
    wid = BI.window_id()
    BI.ledger_append(ledger, [record(cells[2], 1.1, research_window_id=wid, origin="prior")], chain=True)
    lines = ledger.read_text(encoding="utf-8").splitlines()
    records = BI.ledger_read(ledger)                         # the chain verifies through the protocol line
    assert [r["kind"] for r in records] == ["construction", "construction", "protocol", "construction"]
    assert "cell" not in records[2] and records[2]["trial_id"] == proto["trial_id"]
    assert records[3]["prev_sha256"] == BI.line_sha256(lines[2])          # the next line pins the protocol line
    # not counted: N, Appendix A, the cell listings and the variance skip it
    assert BI.trial_counts(records) == [1, 1, 0, 1]
    counts = BI.ledger_counts(records)
    assert "protocol" not in counts and sum(sum(v.values()) for v in counts.values()) == 3
    text = BI.appendix_a(records, "trials.jsonl")
    assert text[0] == "Appendix A (trial ledger trials.jsonl): 3 trials in 4 ledger lines"
    assert text[-1].endswith("1 protocol line(s))")
    names, series = BI.ledger_net_series(records, lambda r: NS.net_series(NS.load_daily_csv(Path(r["series"]["path"]))))
    assert names == [str(c).replace("\\", "/") for c in cells] and len(series) == 3
    assert BI.dsr_variance(records, wid)["n"] == 3
    assert BI.appendix_a_v8(records).startswith("TRAIN construction cells 3;")
    # nav_summ readers: --ledger-n, --effective-n LEDGER and --dsr-ledger run over it
    assert NS.main(["--ledger-n", str(ledger), "--protocol", "v8"]) == 0
    out = capsys.readouterr().out
    assert "Appendix A (trial ledger " in out and "3 trials in 4 ledger lines" in out
    assert "TRAIN construction cells 3;" in out
    assert NS.main([str(cells[0]), "--effective-n", str(ledger), "--dsr-ledger", str(ledger)]) == 0
    out = capsys.readouterr().out
    assert "trial series" in out and "   ledger DSR (N=3, " in out
    # re-appending a ledgered cell after the protocol line adds nothing
    assert BI.ledger_append(ledger, [record(cells[0], 0.5)], chain=True)[0] == []
    # chained: editing or removing the protocol line breaks the next line's link
    tampered = tmp_path / "tampered.jsonl"
    tampered.write_text("\n".join(lines[:2] + [lines[2].replace("hidden", "HIDDEN")] + lines[3:]) + "\n",
                        encoding="utf-8")
    with pytest.raises(ValueError, match="hash chain broken"):
        BI.ledger_read(tampered)
    removed = tmp_path / "removed.jsonl"
    removed.write_text("\n".join(lines[:2] + lines[3:]) + "\n", encoding="utf-8")
    with pytest.raises(ValueError, match="hash chain broken"):
        BI.ledger_read(removed)
    with pytest.raises(SystemExit):                          # nav_summ refuses a broken ledger, it never reads past it
        NS.main([str(cells[0]), "--ledger", str(tampered)])
    # a protocol line that itself carries prev_sha256 (appended through ledger_append) is verified the same way
    own = tmp_path / "own.jsonl"
    BI.ledger_append(own, [record(cells[0], 0.5)], chain=True)
    rec = dict(proto)
    BI.ledger_append(own, [rec], chain=True)
    own_lines = own.read_text(encoding="utf-8").splitlines()
    assert json.loads(own_lines[1])["prev_sha256"] == BI.line_sha256(own_lines[0])
    own.write_text(own_lines[0].replace('"count":1', '"count":2') + "\n" + own_lines[1] + "\n", encoding="utf-8")
    with pytest.raises(ValueError, match="hash chain broken"):
        BI.ledger_read(own)
    # an old (v7, unchained) ledger followed by a protocol line and chained lines reads as before
    old = tmp_path / "old.jsonl"
    BI.ledger_append(old, [record(cells[0], 0.5), record(cells[1], 0.8)])
    lane_a_protocol_line(old, "cd" * 32)
    BI.ledger_append(old, [record(cells[2], 1.1)], chain=True)
    assert BI.trial_counts(BI.ledger_read(old)) == [1, 1, 0, 1]
    old_lines = old.read_text(encoding="utf-8").splitlines()
    assert json.loads(old_lines[3])["prev_sha256"] == BI.chain_head(old_lines[:3])   # pins all three (review C-6)


# ------------------------------------------------------------------ seal, v8 defaults, shim
def test_sealed_series_is_refused_before_any_statistic(tmp_path):
    rw = BI.research_window()
    rows = (rw.SEAL_NS - T0) // DAY + 1                      # CSV rows T0 .. the seal day
    d = cell(tmp_path, "sealed", np.full(rows - 2, 0.001))   # write_nav adds two leading rows
    with pytest.raises(SystemExit, match="research seal"):
        NS.load_daily(d, SCEN)
    with pytest.raises(SystemExit, match="research seal"):
        NS.main([str(d)])
    ok = cell(tmp_path, "ok", np.full(300, 0.001))
    with pytest.raises(SystemExit, match="research seal"):
        NS.main(["--bundle", str(ok), str(d), "--draws", "99"])
    assert NS.load_daily_csv(d / f"daily_{SCEN}.csv", allow_sealed=True)["session_ns"][-1] >= rw.SEAL_NS
    # a series ending the day before the seal is read
    last_ok = cell(tmp_path, "edge", np.full(rows - 3, 0.001))
    assert NS.load_daily(last_ok, SCEN)["session_ns"][-1] == rw.SEAL_NS - DAY


def test_v8_bootstrap_defaults(tmp_path):
    ref = cell(tmp_path, "ref", 0.0002 + 0.01 * np.random.default_rng(1).normal(size=120))
    a = cell(tmp_path, "a", 0.0004 + 0.01 * np.random.default_rng(2).normal(size=120))
    for extra, draws, seed in (([], 2000, 20260927), (["--protocol", "v8"], 4999, 20260929),
                               (["--protocol", "v8", "--seed", "7", "--draws", "300"], 300, 7)):
        out = tmp_path / f"p{len(extra)}.json"
        assert NS.main([str(a), "--reference", str(ref), "--json", str(out)] + extra) == 0
        p = json.loads(out.read_text(encoding="utf-8"))[0]["paired"]
        assert (p["draws"], p["block"], p["seed"]) == (draws, 21, seed)
    assert NS.V8_SEED == 20260929 and NS.V8_DRAWS == 4999 and NS.DEFAULT_BLOCK == 21


def test_old_study_path_is_a_shim_to_the_moved_script(tmp_path):
    assert SHIM.is_file()
    spec = importlib.util.spec_from_file_location("nav_summ_shim_probe", SHIM)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    assert mod.paired_stats is NS.paired_stats or mod.paired_stats.__code__.co_filename == NS.__file__
    done = subprocess.run([PY, str(SHIM), "--help"], capture_output=True, text=True, timeout=120, check=False)
    assert done.returncode == 0 and "--bundle BASE FINAL" in done.stdout
    assert not (SHIM.parent / "backtest_integrity.py").exists()
