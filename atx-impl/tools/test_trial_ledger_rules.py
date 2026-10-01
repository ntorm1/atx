"""Trial ledger rules of backtest_integrity.py fixed after the platform v8 Wave 1 review (area C).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_trial_ledger_rules.py

C-3 defect and re-run flags on a ledgered cell are refused, a defect found later is a defect line.
C-4 rerun_of names an earlier cell line of the same kind; a window re-run's target is on another window.
C-5 a blind (or returns) re-run needs a defect of its target on an earlier line; a re-run never lowers N.
C-6 the hash chain covers the legacy (unchained) lines from the first line of the ledger.
E-33 a mining-campaign line adds 0 to N and carries its own registry count.
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
    extra: dict = {"research_window_id": window_id, "origin": "prior"} if window_id else {}
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


# ------------------------------------------------------------------ C-6
def legacy_ledger(path: Path, n: int = 37) -> list[str]:
    """n legacy construction lines as nav_summ v7 wrote them (unchained, compact sorted-key JSON): the v7 ledger."""
    lines = [json.dumps({"schema": BI.LEDGER_SCHEMA, "kind": "construction", "count": 1, "cell": f"legacy/c{k:02d}",
                         "window": {"label": "TRAIN", "first_session": "2020-01-06", "last_session": "2022-12-30",
                                    "sessions": 754}, "s2_net_sr": 0.5 + 0.01 * k, "trial_id": f"{k:016x}"},
                        sort_keys=True, separators=(",", ":")) for k in range(n)]
    path.write_text("".join(x + "\n" for x in lines), encoding="utf-8")
    return lines


def test_the_chain_covers_the_37_legacy_lines(tmp_path):
    """Review C-6: only the last of the 37 unchained legacy lines was pinned by the first chained line, so adding
    "rerun_basis":"window" to legacy line 5 lowered N by 1 and ledger_read passed. The first chained line now names the
    fold of every line before it (each hashed as stored); an unchained line after a chained one is refused."""
    ledger = tmp_path / "trials.jsonl"
    legacy = legacy_ledger(ledger)
    assert BI.ledger_n(BI.ledger_read(ledger), True) == 37
    head37 = BI.ledger_head(ledger)
    assert head37 == BI.chain_head(legacy) != BI.line_sha256(legacy[-1])            # the fold of all 37
    d = cells(tmp_path, 1)[0]
    appended, _ = BI.ledger_append(ledger, [record(d, 0.7)], chain=True)             # the first v8 line
    assert appended[0]["prev_sha256"] == head37
    text = ledger.read_text(encoding="utf-8").splitlines()
    assert len(BI.ledger_read(ledger)) == 38 and BI.ledger_head(ledger) == BI.line_sha256(text[-1])
    edited = list(text)
    edited[4] = edited[4].replace('"count":1,', '"count":1,"rerun_basis":"window",')  # legacy line 5 out of N
    assert edited[4] != text[4]
    assert BI.ledger_n([json.loads(x) for x in edited], True) == 37                  # what the edit would do
    for name, rows in (("edited", edited), ("removed", text[:11] + text[12:]),
                       ("inserted", text[:20] + [text[20].replace("c20", "cXX")] + text[20:])):
        p = tmp_path / f"{name}.jsonl"
        p.write_text("\n".join(rows) + "\n", encoding="utf-8")
        with pytest.raises(ValueError, match="hash chain broken"):
            BI.ledger_read(p)
    tail = tmp_path / "tail.jsonl"                                                   # after a chained line, never
    tail.write_text("\n".join(text + [legacy[0].replace("c00", "late")]) + "\n", encoding="utf-8")   # unchained
    with pytest.raises(ValueError, match="an unchained line after the chained line 38"):
        BI.ledger_read(tail)
    last = tmp_path / "last.jsonl"                                                   # the tail itself: caught
    last.write_text("\n".join(text[:-1] + [text[-1].replace('"s2_net_sr":0.7', '"s2_net_sr":0.9')]) + "\n",
                    encoding="utf-8")                                                # against a recorded head
    assert BI.ledger_head(last) != BI.ledger_head(ledger)
    only = tmp_path / "only.jsonl"                                                   # before any chained line, the
    only.write_text("\n".join(edited[:37]) + "\n", encoding="utf-8")                 # recorded head catches an edit
    assert BI.ledger_head(only) != head37
    crlf = tmp_path / "crlf.jsonl"                                                   # a CRLF checkout: same chain
    crlf.write_bytes(ledger.read_bytes().replace(b"\n", b"\r\n"))
    assert BI.ledger_head(crlf) == BI.ledger_head(ledger) and len(BI.ledger_read(crlf)) == 38


# ------------------------------------------------------------------ C-5
def trial_counts_before_c5(records: list[dict]) -> list[int]:
    """The defect rule as coded before review C-5 (a blind re-run counted, its replaced target 0), for the identity
    check: on a consistent ledger both give the same N."""
    seen = {r.get("rerun_of") for r in records if r.get("rerun_basis") == "returns"}
    replaced = {r.get("rerun_of") for r in records if r.get("rerun_basis") == "blind"}
    invalid = BI.invalid_ids(records)
    return [0 if BI.is_event(r) or r.get("rerun_basis") == "window" or BI.is_era_line(r) else
            0 if r.get("trial_id") not in seen and (r.get("trial_id") in invalid or r.get("trial_id") in replaced) else
            int(r.get("count", 1)) for r in records]


def test_a_blind_rerun_needs_a_defect_and_never_lowers_n(tmp_path):
    """Review C-5's example: R-3 ledgered (1); R-3 with another parameter ledgered as its blind re-run used to count
    [0, 1] (N unchanged, two results read). Refused now without a defect of R-3 on an earlier line; with one, the pair
    is one trial held by the replaced cell ([1, 0]) and the defect line alone had taken R-3 out of N."""
    c = cells(tmp_path, 5, prefix="r")
    ledger = tmp_path / "trials.jsonl"
    r3 = record(c[0], 0.6)
    BI.ledger_append(ledger, [r3], chain=True)
    for basis in ("blind", "returns"):
        with pytest.raises(ValueError, match=f"a {basis} re-run replaces an invalid cell.*ledger-defect --trial-id"):
            BI.ledger_append(ledger, [record(c[1], 0.9, rerun_of=r3["trial_id"], rerun_basis=basis)], chain=True)
    assert BI.ledger_n(BI.ledger_read(ledger), True) == 1
    BI.ledger_append(ledger, [BI.defect_line(r3["trial_id"], "stale borrow table")], chain=True)
    assert BI.ledger_n(BI.ledger_read(ledger), True) == 0                             # an invalid cell leaves N
    retry = record(c[1], 0.9, rerun_of=r3["trial_id"], rerun_basis="blind")
    BI.ledger_append(ledger, [retry], chain=True)
    records = BI.ledger_read(ledger)
    assert BI.trial_counts(records) == [1, 0, 0] and BI.ledger_n(records, True) == 1   # the re-run raised N back
    v = BI.dsr_variance(records, WID)
    assert v["cells"] == 1 and v["n"] == 1                                             # V: the valid re-run's SR only
    assert BI.appendix_a(records, "t")[-1] == ("   adding no trial: 2 line(s) (2 by the defect rule, 0 window "
                                               "re-run(s), 0 protocol line(s))")
    # a hand-edited blind re-run of a valid cell (refused by ledger_append) never lowers N either
    valid = record(c[2], 0.4)
    hand = [valid, record(c[3], 0.5, rerun_of=valid["trial_id"], rerun_basis="blind")]
    assert BI.trial_counts(hand) == [1, 0] and trial_counts_before_c5(hand) == [0, 1]
    # identity: on a consistent ledger (every re-run legal) N, V[SR] and the excluded lines equal the old rule's
    looked = record(c[4], 0.2, defect="fills priced at the wrong close")
    BI.ledger_append(ledger, [looked], chain=True)
    records = BI.ledger_read(ledger)
    assert sum(BI.trial_counts(records)) == sum(trial_counts_before_c5(records)) == 1
    assert BI.trial_counts(records) != trial_counts_before_c5(records)                # attribution only


# ------------------------------------------------------------------ Ruling E-33
def test_a_mining_campaign_line_adds_nothing_to_n_and_carries_its_registry_count(tmp_path):
    """Ruling E-33: mining-campaign is a ledger kind; its line adds 0 to the construction N (and to every other N) and
    carries its own registry count (the campaign's budget, pre-registration rule 10), printed beside N."""
    assert BI.MINING_CAMPAIGN in BI.LEDGER_KINDS
    c = cells(tmp_path, 2)
    ledger = tmp_path / "trials.jsonl"
    BI.ledger_append(ledger, [record(c[0], 0.6)], chain=True)
    v7_text = BI.appendix_a(BI.ledger_read(ledger), "t")
    camp = BI.campaign_line("mined-q1", "build-equity/mine/q1/registry.jsonl", "ab" * 32, 1000,
                            registry_total=1250, registry_bytes=96048, budget=2048, research_window_id=WID,
                            date="2026-10-02")
    assert camp == {"schema": BI.LEDGER_SCHEMA, "kind": "mining-campaign", "count": 0, "campaign": "mined-q1",
                    "origin": "mined", "budget": 2048, "registry": {"path": "build-equity/mine/q1/registry.jsonl",
                                                    "chain_head": "ab" * 32, "bytes": 96048, "count": 1000,
                                                    "total": 1250},
                    "window_id": WID, "date": "2026-10-02", "trial_id": BI.trial_id("mining-campaign", "ab" * 32)}
    BI.ledger_append(ledger, [camp, record(c[1], 0.9)], chain=True)
    records = BI.ledger_read(ledger)
    assert BI.trial_counts(records) == [1, 0, 1]
    assert BI.ledger_n(records, True) == 2 and BI.ledger_n(records, True, kind="mining-campaign") == 0
    assert BI.campaign_registry_count(records) == 1000
    assert list(BI.ledger_counts(records)) == ["construction"]                          # not a trial of any kind
    assert BI.dsr_variance(records, WID)["cells"] == 2                                   # nor a cell of V[SR]
    text = BI.appendix_a(records, "t")
    assert text[0] == "Appendix A (trial ledger t): 2 trials in 3 ledger lines"
    assert "   mining-campaign     0  (1 campaign line(s), registry count 1000: the campaigns' own budget, not in N)" \
        in text and not any(x.startswith("   adding no trial") for x in text)
    assert not any("mining-campaign" in x for x in v7_text)                              # no line without a campaign
    with pytest.raises(ValueError, match="records a campaign, not a NAV cell"):          # never a NAV cell's kind
        BI.ledger_record("mining-campaign", str(c[0]), c[0] / "summary.json", c[0] / f"daily_{SCEN}.csv", SCEN,
                         NS.net_series(NS.load_daily(c[0], SCEN)), 0.6)
    with pytest.raises(ValueError, match="is not the trial_id of a ledgered cell line"):  # no defect of a campaign
        BI.ledger_append(ledger, [BI.defect_line(camp["trial_id"], "budget overrun")], chain=True)
    for args, needle in ((("", "r", "ab" * 32, 5), "needs a name"), (("m", "r", "xyz", 5), "chain head"),
                         (("m", "r", "ab" * 16, 5), "chain head"),         # review MINE-1: the pre-fix 16-hex head
                         (("m", "r", "ab" * 32, 0), "positive integer")):
        with pytest.raises(ValueError, match=needle):
            BI.campaign_line(*args, registry_total=5, registry_bytes=96048, budget=8)
    with pytest.raises(ValueError, match="byte count"):
        BI.campaign_line("m", "r", "ab" * 32, 5, registry_total=5, registry_bytes=0, budget=8)
    with pytest.raises(ValueError, match="E-33a"):                      # the total is the registry, >= the count
        BI.campaign_line("m", "r", "ab" * 32, 5, registry_total=4, registry_bytes=96048, budget=8)
    for budget in (4, 0, True):                                         # review MINE-4: rule 10's budget covers it
        with pytest.raises(ValueError, match="budget is fixed in advance"):
            BI.campaign_line("m", "r", "ab" * 32, 5, registry_total=5, registry_bytes=96048, budget=budget)
