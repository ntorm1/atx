"""Trial ledger rules of backtest_integrity.py fixed after the platform v8 Wave 1 review (area C).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_trial_ledger_rules.py

C-3 defect and re-run flags on a ledgered cell are refused, a defect found later is a defect line.
C-4 rerun_of names an earlier cell line of the same kind; a window re-run's target is on another window.
Synthetic NAV cells only (test_nav_summ.write_nav: calendar-day sessions from 2020-01-02, inside TRAIN).
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

import backtest_integrity as BI
import nav_summ as NS
from test_nav_summ import SCEN, write_nav

WID = BI.window_id()


def cells(tmp_path: Path, k: int, prefix: str = "c") -> list[Path]:
    rng = np.random.default_rng(len(prefix) + k)
    return [write_nav(tmp_path / f"{prefix}{i}", list(0.0003 * (i + 1) + 0.01 * rng.normal(size=200)),
                      net_sharpe=0.2 * (i + 1)) for i in range(k)]


def record(d: Path, sr: float = 0.5, window_id: str | None = WID, **kw) -> dict:
    extra = {"research_window_id": window_id, "origin": "prior"} if window_id else {}
    return BI.ledger_record("construction", str(d), d / "summary.json", d / f"daily_{SCEN}.csv", SCEN,
                            NS.net_series(NS.load_daily(d, SCEN)), sr, **extra, **kw)


# ------------------------------------------------------------------ C-3
def test_flags_on_a_ledgered_cell_are_refused_not_dropped(tmp_path, capsys):
    """Review C-3: nav_summ --ledger --ledger-defect (or --rerun-of) on a cell the cycle ledgered already used to print
    "appended 0, skipped 1" and exit 0 with the flag lost; it now exits non-zero and names the defect line route. The
    same flags again (a resumed summ step) are still a no-op."""
    c = cells(tmp_path, 2)
    ledger = tmp_path / "trials.jsonl"
    base = [str(c[0]), "--ledger", str(ledger), "--protocol", "v8", "--origin", "prior", "--draws", "9"]
    assert NS.main(base) == 0                                           # the cycle's summ ledgers the cell
    tid = BI.ledger_read(ledger)[0]["trial_id"]
    before = ledger.read_bytes()
    for flags in (["--ledger-defect", "stale fields manifest"],
                  ["--rerun-of", tid, "--rerun-basis", "window"]):
        with pytest.raises(SystemExit, match=f"already ledgered as trial {tid}.*ledger-defect --trial-id {tid}"):
            NS.main(base + flags)
        assert ledger.read_bytes() == before                           # nothing appended
    assert NS.main(base) == 0 and "appended 0, skipped 1" in capsys.readouterr().out   # no flags: a no-op
    bad = [str(c[1]), "--ledger", str(ledger), "--protocol", "v8", "--origin", "prior", "--draws", "9",
           "--ledger-defect", "cost model misread"]
    assert NS.main(bad) == 0                                            # invalid at its first ledgering
    assert NS.main(bad) == 0 and "appended 0, skipped 1" in capsys.readouterr().out   # same flags: a no-op
    with pytest.raises(SystemExit, match="already ledgered"):
        NS.main(bad[:-1] + ["another reason"])                         # other flags: refused


def test_a_defect_found_later_is_a_defect_line(tmp_path):
    """The defect line (count 0, defect_of the cell's trial_id, chained) takes the cell out of N and out of V[SR] as
    the defect flag would have (v8-prereg item 7); a target that is not a ledgered cell, or is invalid already, and a
    second defect line of a cell are refused."""
    c = cells(tmp_path, 4)
    ledger = tmp_path / "trials.jsonl"
    recs = [record(d, sr) for d, sr in zip(c, (0.4, 0.8, 1.2))]
    BI.ledger_append(ledger, recs, chain=True)
    before = BI.ledger_read(ledger)
    assert BI.trial_counts(before) == [1, 1, 1] and BI.dsr_variance(before, WID)["cells"] == 3
    line = BI.defect_line(recs[1]["trial_id"], "role built without the delisting returns", "2026-10-01")
    appended, skipped = BI.ledger_append(ledger, [line], chain=True)
    assert [a["trial_id"] for a in appended] == [line["trial_id"]] and skipped == []
    after = BI.ledger_read(ledger)
    assert after[-1]["kind"] == "defect" and after[-1]["defect_of"] == recs[1]["trial_id"] and "cell" not in after[-1]
    assert after[-1]["count"] == 0 and "prev_sha256" in after[-1]
    assert BI.trial_counts(after) == [1, 0, 1, 0] and BI.ledger_n(after, True) == 2
    assert [r["trial_id"] for r in BI.excluded_lines(after)] == [recs[1]["trial_id"]]
    v = BI.dsr_variance(after, WID)
    assert v["cells"] == 2 and v["variance_sr"] == pytest.approx(float(np.var(np.array([0.4, 1.2]) / np.sqrt(252),
                                                                                ddof=1)), rel=1e-12)
    assert BI.ledger_counts(after) == {"construction": {k: 2 for k in BI.ledger_counts(after)["construction"]}}
    text = BI.appendix_a(after, "t.jsonl")
    assert text[-1] == "   adding no trial: 2 line(s) (2 by the defect rule, 0 window re-run(s), 0 protocol line(s))"
    assert BI.ledger_append(ledger, [line], chain=True)[0] == []      # the same line again: a no-op
    for target, reason, needle in ((recs[1]["trial_id"], "another reason", "has a defect line already"),
                                   ("0" * 16, "typo", "is not the trial_id of a ledgered cell line"),
                                   (line["trial_id"], "an event", "is not the trial_id of a ledgered cell line")):
        with pytest.raises(ValueError, match=needle):
            BI.ledger_append(ledger, [BI.defect_line(target, reason)], chain=True)
    flagged = record(c[3], 0.3, defect="cost model misread")          # ledgered invalid at first
    BI.ledger_append(ledger, [flagged], chain=True)
    with pytest.raises(ValueError, match="was ledgered invalid already"):
        BI.ledger_append(ledger, [BI.defect_line(flagged["trial_id"], "twice")], chain=True)
    with pytest.raises(ValueError, match="needs a reason"):
        BI.defect_line(recs[0]["trial_id"], " ")
    assert json.loads(ledger.read_text(encoding="utf-8").splitlines()[3])["reason"] == \
        "role built without the delisting returns"


# ------------------------------------------------------------------ C-4
def test_rerun_of_must_name_an_earlier_cell_line_of_the_same_kind(tmp_path):
    """Review C-4: `--rerun-of b0a --rerun-basis window` (a cell name), a typo or another kind's id made the line add 0
    whatever it named (N one too low); each is refused now, and a window re-run's target must be on another window."""
    c = cells(tmp_path, 6)
    ledger = tmp_path / "trials.jsonl"
    legacy = record(c[0], 0.9, window_id=None)                          # a v7 cell (no window_id)
    v8 = record(c[1], 1.1)
    universe = BI.ledger_record("universe", str(c[2]), c[2] / "summary.json", c[2] / f"daily_{SCEN}.csv", SCEN,
                                NS.net_series(NS.load_daily(c[2], SCEN)), 0.2)
    BI.ledger_append(ledger, [legacy, v8, universe], chain=True)
    before = ledger.read_bytes()
    tid = legacy["trial_id"]
    typo = tid[:-1] + ("1" if tid[-1] != "1" else "2")
    for target, basis, needle in (("b0a", "window", "is not the trial_id of an earlier construction cell line"),
                                  (typo, "window", "is not the trial_id"),
                                  (universe["trial_id"], "window", "is not the trial_id of an earlier construction"),
                                  (v8["trial_id"], "window", f"was scored on {WID} already")):
        with pytest.raises(ValueError, match=needle):
            BI.ledger_append(ledger, [record(c[3], 1.0, rerun_of=target, rerun_basis=basis)], chain=True)
        assert ledger.read_bytes() == before                            # refused before anything is written
    with pytest.raises(SystemExit, match="rerun_of 'b0a' is not the trial_id"):        # nav_summ exits non-zero
        NS.main([str(c[3]), "--ledger", str(ledger), "--protocol", "v8", "--origin", "prior", "--draws", "9",
                 "--rerun-of", "b0a", "--rerun-basis", "window"])
    ok = record(c[3], 1.0, rerun_of=legacy["trial_id"], rerun_basis="window")   # the legacy cell on the new window
    BI.ledger_append(ledger, [ok], chain=True)
    records = BI.ledger_read(ledger)
    assert BI.trial_counts(records) == [1, 1, 1, 0] and BI.ledger_n(records, True) == 2
    later = record(c[4], 1.2, rerun_of=ok["trial_id"], rerun_basis="window")      # its target's window again
    with pytest.raises(ValueError, match="scored on"):
        BI.ledger_append(ledger, [later], chain=True)
    batch = [record(c[4], 0.7), record(c[5], 0.8, rerun_of="?", rerun_basis="returns")]
    with pytest.raises(ValueError, match="rerun_of '\\?'"):                        # one refusal: nothing appended
        BI.ledger_append(ledger, batch, chain=True)
    assert len(BI.ledger_read(ledger)) == 4
