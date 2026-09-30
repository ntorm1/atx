"""Tests for holdout_gate.py (platform v8 V-2) on synthetic NAV files only.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_holdout_gate.py

Every NAV dir here is generated (sessions dated in the hidden blocks by construction, to exercise the gate); no real
data is read. Block dates come from research_window.json (task W0-1).
"""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np
import pytest

import backtest_integrity as BI
import holdout_gate as HG
import nav_summ as NS
from test_nav_summ import COLUMNS, SCEN

TOOLS = Path(__file__).resolve().parent
DAY = BI.DAY_NS
WEIGHTS = "ab" * 32


def day_ns(d: dt.date) -> int:
    return (d - dt.date(1970, 1, 1)).days * DAY


def ns_day(ns: int) -> dt.date:
    return dt.date(1970, 1, 1) + dt.timedelta(days=ns // DAY)


def write_hidden_nav(d: Path, first: dt.date, nets_of, days: int, weights: str = WEIGHTS) -> Path:
    """A NAV dir whose CSV rows are calendar days from ``first``: row 0 nothing, row 1 deployment, rows 2.. return rows
    with net ``nets_of(session_date, k)``."""
    d.mkdir(parents=True)
    with (d / f"daily_{SCEN}.csv").open("w", newline="", encoding="utf-8") as stream:
        w = csv.writer(stream)
        w.writerow(COLUMNS)
        for k in range(days):
            s = first + dt.timedelta(days=k)
            net = nets_of(s, k) if k >= 2 else 0.0
            w.writerow([k, day_ns(s), int(k >= 1), int(k >= 2), net, 0, 0, 5e8 if k == 1 else 1e7, 1e3,
                        0 if k < 2 else 1e9, 1.0, 0.0, 100, 0.05, "applied", 1e9])
    summary = {"rule": "aim-partial-v5", "status": "complete", "primary_scenario": SCEN,
               "composition_weights_sha256": weights,
               "scenarios": [{"scenario": SCEN, "primary": True, "net_sharpe": 9.99}]}
    (d / "summary.json").write_text(json.dumps(summary), encoding="utf-8")
    return d


def blocks():
    return HG.hidden_blocks()


def good_bad(split_ns: int):
    """Positive drift before ``split_ns`` (the 2025-onward begin), negative drift from it."""
    rng = np.random.default_rng(4)
    noise = rng.normal(size=5000)

    def nets(s: dt.date, k: int) -> float:
        drift = 0.002 if day_ns(s) < split_ns else -0.002
        return drift + 0.005 * float(noise[k])
    return nets


def write_inputs(tmp: Path, nav: Path, *, thresholds=None, deploy=None, ruling=None, at: str = "") -> dict:
    """The three input files in ``tmp/at`` and the trial ledger ``tmp/trials.jsonl`` (shared: review C-11)."""
    thr = {"schema": HG.THRESHOLDS_SCHEMA, "min_sessions": 100, "net_sharpe_min": 1.0}
    thr.update(thresholds or {})
    dep = {"schema": HG.DEPLOY_SCHEMA, "book": "V8-F", "nav_dir": str(nav), "scenario": None,
           "composition_weights_sha256": WEIGHTS}
    dep.update(deploy or {})
    d = tmp / at
    d.mkdir(parents=True, exist_ok=True)
    paths = {"deploy": d / "deploy.json", "thresholds": d / "thresholds.json", "ruling": d / "ruling.json",
             "ledger": tmp / "trials.jsonl"}
    paths["deploy"].write_text(json.dumps(dep), encoding="utf-8")
    paths["thresholds"].write_text(json.dumps(thr), encoding="utf-8")
    rul = {"schema": HG.RULING_SCHEMA, "date": "2027-01-15", "owner": "owner", "text": "open the hidden blocks once",
           "deploy_manifest_sha256": hashlib.sha256(paths["deploy"].read_bytes()).hexdigest(),
           "thresholds_sha256": hashlib.sha256(paths["thresholds"].read_bytes()).hexdigest(),
           "blocks": ["2024", "2025_onward"]}
    rul.update(ruling or {})
    paths["ruling"].write_text(json.dumps(rul), encoding="utf-8")
    return paths


def argv(paths: dict, drop: str | None = None) -> list[str]:
    out = []
    for key, flag in (("deploy", "--deploy"), ("thresholds", "--thresholds"), ("ruling", "--owner-ruling"),
                      ("ledger", "--ledger")):
        if key != drop:
            out += [flag, str(paths[key])]
    return out


def gate(paths: dict) -> dict:
    return HG.gate(paths["deploy"], paths["thresholds"], paths["ruling"], paths["ledger"])


@pytest.fixture
def nav(tmp_path):
    b = blocks()
    first = ns_day(b["2024"][0]) - dt.timedelta(days=40)          # a warm-up tail in TRAIN, then both blocks
    last = ns_day(b["2025_onward"][0]) + dt.timedelta(days=400)
    return write_hidden_nav(tmp_path / "nav", first, good_bad(b["2025_onward"][0]), (last - first).days)


@pytest.fixture
def no_series_read(monkeypatch):
    """Fails the test if the gate opens a daily NAV series."""
    def trap(*a, **k):
        raise AssertionError("a daily NAV series was opened")
    monkeypatch.setattr(NS, "load_daily_csv", trap)


def test_two_bits_and_nothing_else(tmp_path, nav, capsys):
    paths = write_inputs(tmp_path, nav)
    before = sorted(p.relative_to(tmp_path) for p in tmp_path.rglob("*"))
    assert HG.main(argv(paths)) == 0
    out, err = capsys.readouterr()
    assert out == '{"pass_2024": true, "pass_2025_onward": false}\n' and err == ""
    assert list(json.loads(out)) == ["pass_2024", "pass_2025_onward"]
    after = sorted(p.relative_to(tmp_path) for p in tmp_path.rglob("*"))
    assert after == sorted(before + [Path("trials.jsonl")])              # writes nothing but its ledger line
    ledger = paths["ledger"].read_bytes()
    # the same through a fresh interpreter: stdout is exactly the two bits; the same ruling appends nothing
    done = subprocess.run([sys.executable, str(TOOLS / "holdout_gate.py")] + argv(paths), capture_output=True,
                          text=True, timeout=120, check=False)
    assert done.returncode == 0 and done.stdout == out and done.stderr == ""
    assert paths["ledger"].read_bytes() == ledger


def test_a_read_is_recorded_in_the_ledger_citing_the_ruling_sha(tmp_path, nav, capsys):
    """Review C-11: the read leaves a chained validation line with the SHA-256 of the ruling's bytes (before any NAV
    series is opened); the line adds no trial."""
    paths = write_inputs(tmp_path, nav)
    assert HG.main(argv(paths)) == 0
    [line] = BI.ledger_read(paths["ledger"])
    ruling_sha = hashlib.sha256(paths["ruling"].read_bytes()).hexdigest()
    ruling = json.loads(paths["ruling"].read_text(encoding="utf-8"))
    assert line == {"schema": BI.LEDGER_SCHEMA, "kind": "validation", "count": 0, "book": "V8-F",
                    "owner_ruling": {"path": "ruling.json", "sha256": ruling_sha}, "owner": "owner",
                    "date": "2027-01-15", "blocks": ["2024", "2025_onward"],
                    "thresholds_sha256": ruling["thresholds_sha256"],
                    "deploy_manifest_sha256": ruling["deploy_manifest_sha256"],
                    "trial_id": BI.trial_id("validation", ruling_sha), "prev_sha256": BI.CHAIN_GENESIS}
    assert BI.trial_counts([line]) == [0] and BI.ledger_counts([line]) == {} and BI.ledger_n([line], True) == 0
    assert "1 hidden-block validation read(s)" in BI.appendix_a([line], "trials.jsonl")[-1]


def test_an_edited_ruling_is_refused(tmp_path, nav, capsys, monkeypatch):
    """Review C-11: a ruling edited after a read under it (the thresholds bisection of the review: loosen the
    thresholds, re-pin them in the same ruling file) is refused before any series is read; a new ruling file is
    allowed and leaves its own line."""
    paths = write_inputs(tmp_path, nav)
    assert HG.main(argv(paths)) == 0
    capsys.readouterr()
    ledger = paths["ledger"].read_bytes()
    edited = write_inputs(tmp_path, nav, thresholds={"net_sharpe_min": 0.5})         # same ruling path, new bytes
    monkeypatch.setattr(NS, "load_daily_csv", lambda *a, **k: pytest.fail("a daily NAV series was opened"))
    assert HG.main(argv(edited)) == HG.EXIT_REFUSED
    out, err = capsys.readouterr()
    assert out == "" and "bytes differ from the SHA-256 the trial ledger line citing it records" in err
    assert paths["ledger"].read_bytes() == ledger                                     # nothing appended
    name = "RULING.json" if os.name == "nt" else "ruling.json"                        # Windows: case-insensitive
    other = dict(edited, ruling=tmp_path / "sub" / ".." / name)                       # another spelling, same file
    (tmp_path / "sub").mkdir()
    assert HG.main(argv(other)) == HG.EXIT_REFUSED
    assert "bytes differ" in capsys.readouterr().err
    monkeypatch.undo()
    fresh = write_inputs(tmp_path, nav, thresholds={"net_sharpe_min": 0.5}, at="ruling-2")
    assert HG.main(argv(fresh)) == 0
    lines = BI.ledger_read(paths["ledger"])
    assert [x["owner_ruling"]["path"] for x in lines] == ["ruling.json", "ruling-2/ruling.json"]
    assert lines[1]["thresholds_sha256"] != lines[0]["thresholds_sha256"]            # the second read is in the ledger


def test_refuses_without_a_ledger_or_with_a_broken_one(tmp_path, nav, capsys, no_series_read):
    paths = write_inputs(tmp_path, nav)
    assert HG.main(argv(paths, drop="ledger")) == HG.EXIT_REFUSED
    assert "no trial ledger" in capsys.readouterr().err
    paths["ledger"].write_text('{"schema": "atx.trial-ledger/v1", "kind": "protocol", "prev_sha256": "'
                               + "1" * 64 + '"}\n', encoding="utf-8")
    assert HG.main(argv(paths)) == HG.EXIT_REFUSED
    assert "hash chain is broken" in capsys.readouterr().err


def test_blocks_come_from_the_research_window(tmp_path):
    b = blocks()
    hidden = BI.research_window().load()["hidden"]
    assert b["2024"] == (day_ns(dt.date.fromisoformat(hidden["read_twice_at_book_level"][0])),
                         day_ns(dt.date.fromisoformat(hidden["read_twice_at_book_level"][1])))
    assert b["2025_onward"] == (day_ns(dt.date.fromisoformat(hidden["never_read"][0])), None)
    assert b["2024"][0] == BI.research_window().SEAL_NS
    # TRAIN sessions in the file never enter a bit: a crash before the seal leaves both bits as the blocks decide
    first = ns_day(b["2024"][0]) - dt.timedelta(days=200)
    last = ns_day(b["2025_onward"][0]) + dt.timedelta(days=400)
    rng = np.random.default_rng(8)
    noise = rng.normal(size=5000)

    def nets(s, k):
        return -0.05 if day_ns(s) < b["2024"][0] else 0.002 + 0.005 * float(noise[k])
    d = write_hidden_nav(tmp_path / "n", first, nets, (last - first).days)
    paths = write_inputs(tmp_path, d)
    assert gate(paths) == {"pass_2024": True, "pass_2025_onward": True}


def test_refuses_without_owner_ruling(tmp_path, nav, capsys, no_series_read):
    paths = write_inputs(tmp_path, nav)
    assert HG.main(argv(paths, drop="ruling")) == HG.EXIT_REFUSED
    out, err = capsys.readouterr()
    assert out == "" and "refusing: no owner ruling" in err
    paths["ruling"].unlink()
    assert HG.main(argv(paths)) == HG.EXIT_REFUSED
    out, err = capsys.readouterr()
    assert out == "" and "cannot read the owner ruling" in err


@pytest.mark.parametrize("change, message", [
    ({"deploy_manifest_sha256": "0" * 64}, "deploy manifest is not the one"),
    ({"thresholds_sha256": "1" * 64}, "thresholds file is not the one"),
    ({"blocks": ["2024"]}, "must name exactly the blocks"),
    ({"schema": "atx.holdout-owner-ruling/v0"}, "schema"),
    ({"text": "  "}, "has no text"),
    ({"owner": ""}, "has no owner"),
    ({"date": "soon"}, "date"),
    ({"extra": 1}, "unknown keys"),
])
def test_refuses_a_ruling_that_does_not_pin_these_inputs(tmp_path, nav, capsys, no_series_read, change, message):
    paths = write_inputs(tmp_path, nav, ruling=change)
    assert HG.main(argv(paths)) == HG.EXIT_REFUSED
    out, err = capsys.readouterr()
    assert out == "" and message in err


def test_refuses_inputs_changed_after_the_ruling(tmp_path, nav, capsys, no_series_read):
    paths = write_inputs(tmp_path, nav)
    thr = json.loads(paths["thresholds"].read_text(encoding="utf-8"))
    paths["thresholds"].write_text(json.dumps(dict(thr, net_sharpe_min=-5.0)), encoding="utf-8")   # loosened later
    assert HG.main(argv(paths)) == HG.EXIT_REFUSED
    assert "thresholds file is not the one" in capsys.readouterr().err
    paths = write_inputs(tmp_path, nav)
    dep = json.loads(paths["deploy"].read_text(encoding="utf-8"))
    paths["deploy"].write_text(json.dumps(dict(dep, book="other")), encoding="utf-8")
    assert HG.main(argv(paths)) == HG.EXIT_REFUSED
    assert "deploy manifest is not the one" in capsys.readouterr().err


@pytest.mark.parametrize("thresholds, deploy, message", [
    ({"min_sessions": 1}, {}, "min_sessions"),
    ({"net_sharpe_min": "high"}, {}, "net_sharpe_min"),
    ({"max_drawdown_max": 1.5}, {}, "max_drawdown_max"),
    ({"sortino_min": 1.0}, {}, "unknown keys"),
    ({}, {"schema": "atx.holdout-deploy/v0"}, "schema"),
    ({}, {"nav_dir": ""}, "no nav_dir"),
    ({}, {"composition_weights_sha256": "xyz"}, "composition_weights_sha256"),
])
def test_refuses_malformed_inputs(tmp_path, nav, capsys, no_series_read, thresholds, deploy, message):
    paths = write_inputs(tmp_path, nav, thresholds=thresholds, deploy=deploy)
    assert HG.main(argv(paths)) == HG.EXIT_REFUSED
    out, err = capsys.readouterr()
    assert out == "" and message in err


def test_refuses_a_nav_output_that_is_not_the_frozen_book(tmp_path, capsys, no_series_read):
    b = blocks()
    first = ns_day(b["2024"][0])
    d = write_hidden_nav(tmp_path / "other", first, good_bad(b["2025_onward"][0]), 800, weights="cd" * 32)
    paths = write_inputs(tmp_path, d)
    assert HG.main(argv(paths)) == HG.EXIT_REFUSED
    assert "not the frozen book" in capsys.readouterr().err
    paths = write_inputs(tmp_path, d, deploy={"composition_weights_sha256": "cd" * 32, "scenario": "nope"}, at="r2")
    assert HG.main(argv(paths)) == HG.EXIT_REFUSED
    assert "scenario is not in the NAV summary" in capsys.readouterr().err
    paths = write_inputs(tmp_path, tmp_path / "missing", at="r3")
    assert HG.main(argv(paths)) == HG.EXIT_REFUSED
    assert "no summary.json" in capsys.readouterr().err


def test_thresholds_fail_closed(tmp_path, nav):
    b = blocks()
    # too few sessions in a block is a fail, never a pass on thin data
    paths = write_inputs(tmp_path, nav, thresholds={"min_sessions": 10_000})
    assert gate(paths) == {"pass_2024": False, "pass_2025_onward": False}
    # a 2025-onward block absent from the file fails
    first = ns_day(b["2024"][0])
    short = write_hidden_nav(tmp_path / "short", first, good_bad(b["2025_onward"][0]),
                             (ns_day(b["2025_onward"][0]) - first).days)
    paths = write_inputs(tmp_path, short, at="short-ruling")
    assert gate(paths) == {"pass_2024": True, "pass_2025_onward": False}
    # drawdown and compounded-return bounds
    x = np.r_[np.full(150, 0.01), np.full(30, -0.02), np.full(150, 0.01)]
    thr = {"min_sessions": 100, "net_sharpe_min": 0.0}
    assert HG.block_passes(x, thr)
    assert not HG.block_passes(x, dict(thr, max_drawdown_max=0.30))       # 1 - 0.98^30 = 0.455
    assert HG.block_passes(x, dict(thr, max_drawdown_max=0.50))
    assert not HG.block_passes(x, dict(thr, net_return_min=100.0))
    assert not HG.block_passes(np.full(200, 0.001), thr)                  # zero volatility: undefined SR, a fail
    assert not HG.block_passes(np.r_[x, np.nan], thr)


def test_evaluation_failure_prints_no_statistic(tmp_path, nav, capsys):
    paths = write_inputs(tmp_path, nav)
    with (nav / f"daily_{SCEN}.csv").open("a", encoding="utf-8") as f:
        f.write("broken,row\n")
    assert HG.main(argv(paths)) == HG.EXIT_FAILED
    out, err = capsys.readouterr()
    assert out == "" and err == "holdout_gate: the NAV output could not be evaluated (no result)\n"
