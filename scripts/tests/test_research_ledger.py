"""One trial count N and one ledger append for research_cycle.py and nav_summ.py (platform v8 lane G, task 1).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_ledger.py

PM ruling 2026-09-29: the N that gates is the validation kit's defect-rule count (backtest_integrity.trial_counts;
v8-prereg Appendix A rules 2 and 7), and a protocol line is written through nav_summ's chained append. Synthetic NAV
cells only (atx-impl/tools/test_nav_summ.write_nav, sessions in 2020); the research window is task W0-1's.
"""
from __future__ import annotations

import hashlib
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
    ruling = lambda x: BI.defect_line(x["trial_id"], x["defect"]["reason"], "2026-10-01", "E-31")   # noqa: E731
    lines = [a, b,
             record(root, "prior/c", rerun_of=a["trial_id"], rerun_basis="window"),   # a on the longer window: 0
             ruling(b),                                                                # the ruling b's re-run needs
             record(root, "prior/d", rerun_of=b["trial_id"], rerun_basis="blind"),    # replaces invalid b: b 1, d 0
             e, ruling(e),
             record(root, "prior/f", rerun_of=e["trial_id"], rerun_basis="returns")]  # e stays a trial: 1 + 1
    BI.ledger_append(ledger, lines, chain=True)
    assert protocol(ledger, root) == 0                                                 # protocol line: 0
    records = BI.ledger_read(ledger)
    assert BI.trial_counts(records) == [1, 1, 0, 0, 0, 1, 0, 1, 0]                     # review C-5 attribution
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
            "--ruling", "E-31", "--date", "2026-10-01", "--root", str(root)]
    for missing in ("--ruling", "--date"):                              # review F-5: both are required
        k = argv.index(missing)
        with pytest.raises(SystemExit):
            RC.main(argv[:k] + argv[k + 2:])
    capsys.readouterr()
    assert RC.main(argv) == 0 and capsys.readouterr().out.startswith("appended: ")
    assert RC.main(argv) == 0 and capsys.readouterr().out.startswith("already present")
    records = BI.ledger_read(ledger)
    assert records[-1]["kind"] == "defect" and records[-1]["defect_of"] == b["trial_id"] and "prev_sha256" in records[-1]
    assert (records[-1]["ruling"], records[-1]["date"]) == ("E-31", "2026-10-01")
    assert BI.trial_counts(records) == [1, 0, 0] and cycle_n(root, sp) == 2
    assert research_ledger.cells(ledger) == ["prior/a", "prior/b"]      # the cell listing skips the event line
    BI.ledger_append(ledger, [BI.campaign_line("mined-q1", "mine/registry.jsonl", "cd" * 32, 250,     # Ruling E-33
                                               registry_total=250, registry_bytes=4096, budget=400,
                                               recipe_sha256="ef" * 32,
                                               confirm={"begin": "2023-01-01", "end": "2024-01-01"}),
                              {"schema": BI.LEDGER_SCHEMA, "kind": "validation", "count": 0,       # review C-11
                               "owner_ruling": {"path": "r.json", "sha256": "ef" * 32}, "trial_id": "v" * 16}],
                     chain=True)
    assert research_ledger.cells(ledger) == ["prior/a", "prior/b"] and cycle_n(root, sp) == 2      # neither adds
    for bad in (["--trial-id", "0" * 16], ["--date", "01/10/2026"], ["--reason", " "], ["--ruling", " "]):
        args = list(argv)
        args[args.index(bad[0]) + 1] = bad[1]
        assert RC.main(args) == 2


REGISTRY_REL = "mine/registry-w1a.atxtrg"
HEADER_BYTES, RECORD_BYTES = 48, 96                              # a V3 log's header and screened-record frame


def grow_registry(root: Path, rel: str, records: int) -> tuple[str, int]:
    """Appends ``records`` 96-byte records to the registry log at root/rel (created with its 48-byte header) and returns
    (SHA-256 of the whole log, its byte count): the verb's chain head and byte count after a campaign."""
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        path.write_bytes(b"ATXTRG03" + bytes(HEADER_BYTES - 8))
    start = path.stat().st_size
    with open(path, "ab") as f:
        for k in range(records):
            f.write(hashlib.sha256(f"{start}:{k}".encode()).digest() * 3)
    data = path.read_bytes()
    return hashlib.sha256(data).hexdigest(), len(data)


def mine_recipe(tag: str, confirm: tuple[str, str]) -> dict:
    """A campaign's trial recipe in the verb's layout (strategy_mine.cpp recipe_json); ``tag`` stands for the pool."""
    return {"schema": "atx.mine-trial/v1", "rule": "mined-v1", "role_manifest_sha256": "a1" * 32,
            "fields_manifest_sha256": "b2" * 32, "pool_sha256": hashlib.sha256(tag.encode()).hexdigest(),
            "library": {"vm_identity": "dslvm1_clang18.1", "dsl_vm_sources_sha256": "c3" * 32,
                        "ic_identity": "icres1", "ic_sources_sha256": "d4" * 32},
            "window_id": BI.window_id(),
            "discover": {"begin": "2020-01-01", "end": confirm[0], "rows": [383, 1295], "label_rows": 890},
            "confirm": {"begin": confirm[0], "end": confirm[1], "rows": [1295, 1844], "label_rows": 527},
            "ic": "research_window_ic_config (EquivalenceV3, ...)", "marginal": "combine::marginal_rank_ic_day ...",
            "overlap_bands": [[100, 1.47], [1000, 1.54], [10000, 1.63]],
            "confirm_bands": [[16, 1.77], [64, 1.96], [256, 2.15]],
            "max_budget": 10000, "min_discover_rows": 504, "min_confirm_rows": 200,
            "min_names": 10, "min_dates": 128}


def mine_output(root: Path, rel: str, records: int, campaign_id: str = "fixture", registry_rel: str = REGISTRY_REL,
                budget: int = 128, recipe_tag: str | None = None,
                confirm: tuple[str, str] = ("2022-07-01", "2024-01-01"), **ledger_override) -> Path:
    """An atx-equity-strategy-mine output directory exactly as the verb writes it (strategy_mine.cpp and
    strategy_mine_ledger.cpp; review MINE-1), after the campaign appended ``records`` records to its registry:
    campaign.json, registry_head.txt (eval::write_chain_head's line) and ledger_line.json (compact sorted keys), whose
    line is built here independently of backtest_integrity.campaign_line, as the C++ verb builds it. Ruling E-33a:
    the line's count is ``records``, its total the registry's size (a registry shared with earlier campaigns). Review
    MINE-3: the recipe (one per ``recipe_tag``, default ``rel``) binds the confirm window; its SHA-256 is the campaign's
    identity in the line."""
    head, size = grow_registry(root, registry_rel, records)
    total = (size - HEADER_BYTES) // RECORD_BYTES
    directory = root / rel
    directory.mkdir(parents=True)
    chain = hashlib.sha256(head.encode()).hexdigest()[:16]
    recipe = mine_recipe(rel if recipe_tag is None else recipe_tag, confirm)
    recipe_sha = hashlib.sha256(json.dumps(recipe, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    campaign = {"schema": "atx.mine-campaign/v1", "status": "complete", "campaign_id": campaign_id,
                "rule": "mined-v1", "budget": budget,
                "research_window": {"id": BI.window_id(), "seal_begin": "2024-01-01"},
                "recipe_sha256": recipe_sha, "recipe": recipe,
                "registry": {"path": registry_rel, "format": "V3", "records": total,
                             "chain": chain, "head": head, "bytes": size, "n_raw": total, "new_records": records,
                             "anchor": None if total == records else {"records": total - records, "head": "0" * 16}}}
    (directory / "campaign.json").write_text(json.dumps(campaign, indent=2) + "\n", encoding="utf-8")
    (directory / "registry_head.txt").write_text(f"ATXTRGH1 {total:x} {int(chain, 16):x} 1f2e3d\n", encoding="utf-8")
    ident = hashlib.sha256(json.dumps(["mining-campaign", recipe_sha, head], separators=(",", ":")).encode())
    line = {"schema": "atx.trial-ledger/v1", "kind": "mining-campaign", "count": 0, "campaign": campaign_id,
            "origin": "mined", "rule": "mined-v1", "budget": budget, "recipe_sha256": recipe_sha,
            "confirm": {"begin": confirm[0], "end": confirm[1]}, "window_id": BI.window_id(),
            "trial_id": ident.hexdigest()[:16],
            "registry": {"path": registry_rel, "chain_head": head, "bytes": size, "count": records, "total": total}}
    line.update(ledger_override)
    (directory / "ledger_line.json").write_text(json.dumps(line, sort_keys=True, separators=(",", ":")) + "\n",
                                                encoding="utf-8")
    return directory


def test_ledger_campaign_appends_the_mine_verbs_campaign_line(tmp_path, capsys):
    """Ruling E-33 (FIX-C) x H-3 x review MINE-1: `research_cycle.py ledger-campaign` appends the line the mine verb
    writes (its real format: a 64-hex chain head = SHA-256 of the registry's first bytes) through
    backtest_integrity.campaign_line, chained; it equals the verb's ledger_line.json, adds 0 to every N and carries
    the registry count (n_raw); a registry extended by a later campaign still verifies; the same registry head is never
    appended twice. Refused, appending nothing: the pre-fix verb's 16-hex head, a line that differs, a budget above
    kMinedMaxBudget (Ruling PM4-13), an edited registry,
    a registry_head.txt of another head, a missing registry, an incomplete campaign and a missing output."""
    root, sp = make_root(tmp_path, summ={"script": "scripts/summ.py", "dsr_n": "ledger+1", "ledger": "trials.jsonl"})
    ledger = root / "trials.jsonl"
    nav_cell(root, "prior/a", 0)
    BI.ledger_append(ledger, [record(root, "prior/a")], chain=True)
    assert cycle_n(root, sp) == 2
    out = mine_output(root, "mine/out-w1a", 81)
    campaign = json.loads((out / "campaign.json").read_text(encoding="utf-8"))
    grow_registry(root, REGISTRY_REL, 5)                          # a later campaign appends: the prefix still verifies
    argv = ["ledger-campaign", "--ledger", "trials.jsonl", "--campaign", "mine/out-w1a", "--root", str(root)]
    head_before = BI.ledger_head(ledger)
    assert RC.main(argv) == 0 and capsys.readouterr().out.startswith("appended: ")
    records = BI.ledger_read(ledger)
    line = records[-1]
    verb = json.loads((out / "ledger_line.json").read_text(encoding="utf-8"))
    assert {k: v for k, v in line.items() if k != "prev_sha256"} == verb
    reg = campaign["registry"]
    assert len(line["registry"]["chain_head"]) == 64
    assert line == dict(BI.campaign_line("fixture", reg["path"], reg["head"], 81, registry_total=81,
                                         registry_bytes=reg["bytes"], budget=128,
                                         recipe_sha256=campaign["recipe_sha256"],
                                         confirm={"begin": "2022-07-01", "end": "2024-01-01"},
                                         research_window_id=BI.window_id()),
                        prev_sha256=head_before)
    assert line["budget"] == campaign["budget"] == 128                                       # Ruling E-32a
    assert line["confirm"] == {"begin": "2022-07-01", "end": "2024-01-01"}                 # review MINE-3
    assert line["trial_id"] == BI.campaign_trial_id(campaign["recipe_sha256"], reg["head"])
    assert BI.trial_counts(records) == [1, 0] and BI.campaign_registry_count(records) == 81 and cycle_n(root, sp) == 2
    assert research_ledger.cells(ledger) == ["prior/a"]
    before = ledger.read_bytes()
    bad = list(argv)

    def refused(rel: str, needle: str) -> None:
        bad[bad.index("--campaign") + 1] = rel
        assert RC.main(bad) == 2 and needle in capsys.readouterr().err, rel
        assert ledger.read_bytes() == before

    # Review MINE-3: a second confirm read on the same identity is refused, not skipped -- the same output again, a
    # re-run on a fresh registry with the same recipe, or another recipe under the ledgered campaign name.
    refused("mine/out-w1a", "a second confirm read on the same identity is refused")
    mine_output(root, "mine/again", 81, campaign_id="fixture-again", registry_rel="mine/fresh.atxtrg",
                recipe_tag="mine/out-w1a")
    refused("mine/again", "recipe_sha256 with the ledgered campaign 'fixture'")
    mine_output(root, "mine/renamed", 7, registry_rel="mine/r-name.atxtrg")
    refused("mine/renamed", "shares campaign")
    recut = mine_output(root, "mine/recut", 7, campaign_id="recut", registry_rel="mine/r-recut.atxtrg")
    text = (recut / "campaign.json").read_text(encoding="utf-8").replace('"end": "2024-01-01"', '"end": "2023-12-01"')
    (recut / "campaign.json").write_text(text, encoding="utf-8")
    refused("mine/recut", "does not hash to its recipe_sha256")                              # MINE-13: windows edited

    sixteen = mine_output(root, "mine/sixteen", 3, registry_rel="mine/r16.atxtrg")           # the pre-fix verb's head
    head64 = json.loads((sixteen / "campaign.json").read_text(encoding="utf-8"))["registry"]["head"]
    for name in ("campaign.json", "ledger_line.json"):
        text = (sixteen / name).read_text(encoding="utf-8")
        (sixteen / name).write_text(text.replace(head64, head64[:16]), encoding="utf-8")
    refused("mine/sixteen", "chain head")
    mine_output(root, "mine/old-form", 4, registry_rel="mine/r-old.atxtrg", count=99)       # a line that differs
    refused("mine/old-form", "differs from campaign_line on count")
    mine_output(root, "mine/overspent", 4, registry_rel="mine/r-over.atxtrg", budget=3)     # rule 10: over budget
    refused("mine/overspent", "budget is fixed in advance")
    mine_output(root, "mine/over-ceiling", 4, registry_rel="mine/r-ceil.atxtrg", budget=10001)
    refused("mine/over-ceiling", "at most 10000 (kMinedMaxBudget, Ruling PM4-13")           # above F's validation
    edited = mine_output(root, "mine/edited", 4, registry_rel="mine/r-edit.atxtrg")
    log = bytearray((root / "mine" / "r-edit.atxtrg").read_bytes())
    log[60] ^= 1
    (root / "mine" / "r-edit.atxtrg").write_bytes(bytes(log))
    refused("mine/edited", "do not hash to the campaign's chain head")
    sidecar = mine_output(root, "mine/sidecar", 4, registry_rel="mine/r-side.atxtrg")
    (sidecar / "registry_head.txt").write_text("ATXTRGH1 3 abc 1\n", encoding="utf-8")
    refused("mine/sidecar", "registry_head.txt")
    mine_output(root, "mine/gone", 4, registry_rel="mine/r-gone.atxtrg")
    (root / "mine" / "r-gone.atxtrg").unlink()
    refused("mine/gone", "is not readable")
    partial = mine_output(root, "mine/partial", 4, registry_rel="mine/r-part.atxtrg")
    text = (partial / "campaign.json").read_text(encoding="utf-8").replace('"complete"', '"running"')
    (partial / "campaign.json").write_text(text, encoding="utf-8")
    refused("mine/partial", "not a complete")
    refused("mine/absent", "campaign.json")


def test_campaigns_sharing_a_registry_count_their_own_records(tmp_path, capsys):
    """Ruling E-33a (review MINE-5): two campaigns on one registry, the second after the first. Each line carries the
    records its campaign added as registry.count and the registry's size as registry.total, so the ledger's campaign
    registry count is the registry's size once (the pre-E-33a lines counted the first campaign's 81 twice: 81 + 100)."""
    root, sp = make_root(tmp_path, summ={"script": "scripts/summ.py", "dsr_n": "ledger+1", "ledger": "trials.jsonl"})
    ledger = root / "trials.jsonl"
    nav_cell(root, "prior/a", 0)
    BI.ledger_append(ledger, [record(root, "prior/a")], chain=True)
    mine_output(root, "mine/out-a", 81, campaign_id="campaign-a")
    mine_output(root, "mine/out-b", 19, campaign_id="campaign-b")
    for rel in ("mine/out-a", "mine/out-b"):
        argv = ["ledger-campaign", "--ledger", "trials.jsonl", "--campaign", rel, "--root", str(root)]
        assert RC.main(argv) == 0 and capsys.readouterr().out.startswith("appended: "), rel
    records = BI.ledger_read(ledger)
    a, b = records[-2]["registry"], records[-1]["registry"]
    assert (a["count"], a["total"], b["count"], b["total"]) == (81, 81, 19, 100)
    assert BI.campaign_registry_count(records) == 100 == b["total"]
    assert BI.trial_counts(records) == [1, 0, 0] and cycle_n(root, sp) == 2


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
