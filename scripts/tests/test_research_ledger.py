"""One trial count N and one ledger append for research_cycle.py and nav_summ.py (platform v8 lane G, task 1).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_ledger.py

PM ruling 2026-09-29: the N that gates is the validation kit's defect-rule count (backtest_integrity.trial_counts;
v8-prereg Appendix A rules 2 and 7), and a protocol line is written through nav_summ's chained append. Synthetic NAV
cells only (atx-impl/tools/test_nav_summ.write_nav, sessions in 2020); the research window is task W0-1's.
"""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parents[1] / "atx-impl" / "tools"
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(TOOLS))
import backtest_integrity as BI  # noqa: E402
import nav_summ as NS  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_ledger  # noqa: E402
from test_nav_summ import SCEN, write_nav  # noqa: E402
from test_research_cycle import cycle_of, make_root  # noqa: E402

RULING = "expand TRAIN to include 2023; keep 2024+ hidden and out of sample"


def nav_cell(root: Path, rel: str, seed: int) -> Path:
    nets = 0.0004 + 0.01 * np.random.default_rng(seed).normal(size=120)
    return write_nav(root / rel, list(nets))


def record(root: Path, rel: str, legacy: bool = False, **kw) -> dict:
    """A v8 cell line (origin, window_id), or with ``legacy`` a v7 line (neither)."""
    d = root / rel
    v8 = {} if legacy else {"origin": "prior", "research_window_id": BI.window_id()}
    return BI.ledger_record("construction", rel, d / "summary.json", d / f"daily_{SCEN}.csv", SCEN,
                            NS.net_series(NS.load_daily(d, SCEN)), 0.5, **v8, **kw)


def protocol(ledger: Path, root: Path) -> int:
    window = root / "research_window.json"
    if not window.exists():
        window.write_text('{"schema": "atx.research-window/v2"}\n')
    return RC.main(["ledger-protocol", "--ledger", str(ledger), "--owner-ruling", RULING, "--date", "2026-09-29",
                    "--window-id", "research-window-v2", "--research-window", str(window), "--root", str(root)])


def nav_summ_n(root: Path, ledger: Path, rel: str) -> int:
    """nav_summ --dsr-ledger's N for one dir, with its own in-ledger rule (the trial_id of the dir's daily series)."""
    records = BI.ledger_read(ledger)
    present = {r.get("trial_id") for r in records if r.get("kind") == "construction"}
    daily = root / rel / f"daily_{SCEN}.csv"
    moments = NS.net_moments(np.array(list(NS.net_series(NS.load_daily(root / rel, SCEN)).values())))
    tid = BI.trial_id("construction", BI.sha256_file(daily))
    return NS.ledger_dsr(moments, records, tid in present, BI.window_id())["n"]


def cycle_n(root: Path, sp: Path) -> int:
    argv = next(s for s in cycle_of(root, sp).steps() if s.phase == "summ").argv
    return int(argv[argv.index("--dsr-n") + 1])


def test_dsr_n_equals_trial_counts_with_defect_and_rerun_lines(tmp_path):
    root, sp = make_root(tmp_path, summ={"script": "scripts/summ.py", "dsr_n": "ledger+1", "ledger": "trials.jsonl"})
    ledger = root / "trials.jsonl"
    for k, rel in enumerate(("prior/a", "prior/b", "prior/c", "prior/d", "prior/e", "prior/f")):
        nav_cell(root, rel, k)
    a, b, e = record(root, "prior/a", legacy=True), \
        record(root, "prior/b", defect="role built without the delisting returns"), \
        record(root, "prior/e", defect="cost model misread")
    lines = [a, b,
             record(root, "prior/c", rerun_of=a["trial_id"], rerun_basis="window"),   # a on the longer window: 0
             record(root, "prior/d", rerun_of=b["trial_id"], rerun_basis="blind"),    # replaces invalid b: b 1, d 0
             e,
             record(root, "prior/f", rerun_of=e["trial_id"], rerun_basis="returns")]  # e stays a trial: 1 + 1
    BI.ledger_append(ledger, lines, chain=True)
    assert protocol(ledger, root) == 0                                                 # protocol line: 0
    records = BI.ledger_read(ledger)
    assert BI.trial_counts(records) == [1, 1, 0, 0, 1, 1, 0]                           # review C-5 attribution
    want = sum(BI.trial_counts(records)) + 1                                           # + this cycle's cell
    assert want == 5 and len(research_ledger.cells(ledger)) + 1 == 7                   # the old line count differs
    assert cycle_n(root, sp) == want == BI.ledger_n(records, False)                   # plan time: no NAV output yet
    nav_cell(root, "out/N", 99)                                                        # this cycle's NAV output
    assert cycle_n(root, sp) == nav_summ_n(root, ledger, "out/N") == want
    shutil.copytree(root / "out" / "N", root / "out" / "N-first")                     # an identity run under another
    first = record(root, "out/N-first")                                                # name, ledgered first: the same
    BI.ledger_append(ledger, [first])                                                  # series is the same trial
    assert research_ledger.scored_trial_id(root / "out" / "N") == first["trial_id"]
    assert cycle_n(root, sp) == nav_summ_n(root, ledger, "out/N") == want              # counted once, by both
    BI.ledger_append(ledger, [record(root, "out/N")])                                  # nav_summ's own append: a no-op
    assert cycle_n(root, sp) == nav_summ_n(root, ledger, "out/N") == want
    spec = json.loads(sp.read_text())                                                  # an integer N must equal it
    spec["summ"].update(dsr_n=want + 1, cells_from_ledger=True)
    with pytest.raises(RC.CycleError) as err:
        RC.Cycle(spec, RC.Resolver(root)).steps()
    assert err.value.code == RC.EXIT_PIN and f"set summ.dsr_n to {want}" in str(err.value)


def test_ledger_defect_marks_a_ledgered_cell_invalid(tmp_path, capsys):
    """Review C-3: `research_cycle.py ledger-defect` appends one chained defect line for a cell ledgered already; N
    (the cycle's ledger+1 and nav_summ's) drops the cell; research_ledger.cells skips the line; bad input exits 2."""
    root, sp = make_root(tmp_path, summ={"script": "scripts/summ.py", "dsr_n": "ledger+1", "ledger": "trials.jsonl"})
    ledger = root / "trials.jsonl"
    for k, rel in enumerate(("prior/a", "prior/b")):
        nav_cell(root, rel, k)
    a, b = record(root, "prior/a"), record(root, "prior/b")
    BI.ledger_append(ledger, [a, b], chain=True)
    assert cycle_n(root, sp) == 3
    argv = ["ledger-defect", "--ledger", "trials.jsonl", "--trial-id", b["trial_id"], "--reason", "stale fields",
            "--date", "2026-10-01", "--root", str(root)]
    assert RC.main(argv) == 0 and capsys.readouterr().out.startswith("appended: ")
    assert RC.main(argv) == 0 and capsys.readouterr().out.startswith("already present")
    records = BI.ledger_read(ledger)
    assert records[-1]["kind"] == "defect" and records[-1]["defect_of"] == b["trial_id"] and "prev_sha256" in records[-1]
    assert BI.trial_counts(records) == [1, 0, 0] and cycle_n(root, sp) == 2
    assert research_ledger.cells(ledger) == ["prior/a", "prior/b"]      # the cell listing skips the event line
    BI.ledger_append(ledger, [BI.campaign_line("mined-q1", "mine/registry.jsonl", "cd" * 32, 250),   # Ruling E-33
                              {"schema": BI.LEDGER_SCHEMA, "kind": "validation", "count": 0,       # review C-11
                               "owner_ruling": {"path": "r.json", "sha256": "ef" * 32}, "trial_id": "v" * 16}],
                     chain=True)
    assert research_ledger.cells(ledger) == ["prior/a", "prior/b"] and cycle_n(root, sp) == 2      # neither adds
    for bad in (["--trial-id", "0" * 16], ["--date", "01/10/2026"], ["--reason", " "]):
        args = list(argv)
        args[args.index(bad[0]) + 1] = bad[1]
        assert RC.main(args) == 2


def test_the_gate_ledgers_the_admission_trials(tmp_path):
    """Review C-7: v8 Appendix A printed "admission trials this sprint 0" on every result (no writer). The gate of a
    v8 cycle with a ledger appends one chained admission line per listed candidate; a resumed gate or the full run
    after --screen adds nothing; research_ledger.cells skips the lines and N is unchanged; a v7 cycle writes none."""
    import test_research_cycle as T
    summ = {"script": "scripts/summ.py", "dsr_n": "ledger+1", "ledger": "trials.jsonl", "origin": "grid"}
    root, sp = T.screen_root(tmp_path / "v8", summ=summ, verdict=True)
    ledger = root / "trials.jsonl"
    ledger.write_text(T.cell_line("prior/a") + T.cell_line("prior/b"), encoding="utf-8")
    old = BI.appendix_a_v8(BI.ledger_read(ledger))
    n_before = cycle_n(root, sp)
    log: list[str] = []
    assert RC.run_cycle(cycle_of(root, sp, screen=True, capabilities=T.CAPS), log=log.append,
                        clean=lambda r: True) == RC.EXIT_OK
    records = BI.ledger_read(ledger)
    assert [r["kind"] for r in records] == ["construction", "construction", "admission"]
    line = records[-1]
    assert (line["candidate"], line["origin"], line["status"], line["window_id"], line["count"]) == (
        "new_alpha", "grid", "admitted", BI.window_id(), 1)
    text = ledger.read_text(encoding="utf-8").splitlines()
    assert line["prev_sha256"] == BI.chain_head(text[:2]) and line["pins"]["role_sha256"] == \
        RC.sha256_file(root / "role" / "manifest.json")
    assert "1 admission trial line(s) appended, 0 already ledgered" in " ".join(log)
    assert "admission trials this sprint 0;" in old
    assert "admission trials this sprint 1;" in BI.appendix_a_v8(records)           # C-7: 0 -> 1 on this fixture
    assert research_ledger.cells(ledger) == ["prior/a", "prior/b"] and cycle_n(root, sp) == n_before == 3
    v = json.loads((root / "build-equity" / "cycle-synthetic" / "cycle_verdict.json").read_text())
    assert v["ledger"] == {"path": "trials.jsonl", "head": BI.line_sha256(text[2]), "lines": 3}
    assert T.run(root, sp, capabilities=T.CAPS) == RC.EXIT_OK                        # the full run: gate again
    assert len(BI.ledger_read(ledger)) == 3
    # the alpha registry's origin class wins over summ.origin (contract K5)
    root2, sp2 = T.screen_root(tmp_path / "reg", summ=summ, verdict=True)
    reg = root2 / "atx-impl" / "strategies" / "alphas" / "registry.json"
    reg.parent.mkdir(parents=True)
    reg.write_text(json.dumps({"alphas": [{"id": "new_alpha", "origin": "mined"}]}), encoding="utf-8")
    (root2 / "trials.jsonl").write_text(T.cell_line("prior/a"), encoding="utf-8")
    assert RC.run_cycle(cycle_of(root2, sp2, screen=True, capabilities=T.CAPS), log=lambda s: None,
                        clean=lambda r: True) == RC.EXIT_OK
    assert BI.ledger_read(root2 / "trials.jsonl")[-1]["origin"] == "mined"
    # a v7 cycle (no verdict, no --protocol v8) ledgers no admission trial
    root3, sp3 = T.screen_root(tmp_path / "v7", summ={"script": "scripts/summ.py", "dsr_n": 29,
                                                      "ledger": "trials.jsonl"})
    (root3 / "trials.jsonl").write_text(T.cell_line("prior/a"), encoding="utf-8")
    assert T.run(root3, sp3, capabilities=T.CAPS) == RC.EXIT_OK
    assert [r["kind"] for r in BI.ledger_read(root3 / "trials.jsonl")] == ["construction"]


def test_protocol_line_is_chained_when_written(tmp_path):
    root = tmp_path
    ledger = root / "trials.jsonl"
    for k, rel in enumerate(("prior/a", "prior/b", "prior/c")):
        nav_cell(root, rel, k)
    BI.ledger_append(ledger, [record(root, "prior/a"), record(root, "prior/b")])      # v7 lines: unchained
    assert "prev_sha256" not in BI.ledger_read(ledger)[-1]
    assert protocol(ledger, root) == 0 and protocol(ledger, root) == 0                 # the second call adds nothing
    text = ledger.read_text(encoding="utf-8").splitlines()
    assert len(text) == 3
    line = json.loads(text[2])
    assert line["kind"] == "protocol" and line["count"] == 0 and "cell" not in line
    assert line["prev_sha256"] == BI.chain_head(text[:2])                              # linked when written: it
    assert line["prev_sha256"] != BI.line_sha256(text[1])                              # pins both legacy lines (C-6)
    assert text[2] == json.dumps(line, sort_keys=True, separators=(",", ":"))         # nav_summ's encoding
    assert len(BI.ledger_read(ledger)) == 3 and BI.ledger_head(ledger) == BI.line_sha256(text[2])
    appended, _ = BI.ledger_append(ledger, [record(root, "prior/c")])                 # a later nav_summ append
    assert appended[0]["prev_sha256"] == BI.line_sha256(text[2])                       # continues the chain
    for k in (0, 1):                                                                   # either legacy line edited
        tampered = text[k].replace('"s2_net_sr":0.5', '"s2_net_sr":0.6')
        assert tampered != text[k]
        rows = list(text)
        rows[k] = tampered
        edited = root / f"edited{k}.jsonl"
        edited.write_text("\n".join(rows + ledger.read_text().splitlines()[3:]) + "\n", encoding="utf-8")
        with pytest.raises(ValueError, match=rf"edited{k}.jsonl:3: hash chain broken"):   # the protocol line
            BI.ledger_read(edited)                                                          # guards both
    ledger.write_text("\n".join([text[0], text[1].replace('"s2_net_sr":0.5', '"s2_net_sr":0.6'),
                                 *ledger.read_text().splitlines()[2:]]) + "\n", encoding="utf-8")
    before = ledger.read_bytes()
    window = root / "research_window.json"
    window.write_text('{"schema": "atx.research-window/v2", "note": "another window"}\n')
    assert protocol(ledger, root) == 2 and ledger.read_bytes() == before              # a broken chain: refused
