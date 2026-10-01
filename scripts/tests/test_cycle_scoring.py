"""The cycle's scoring step and verdict (platform v8 review W1-C: C-1 verdict DSR, C-2 v8 protocol in summ).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_cycle_scoring.py

Synthetic NAV cells only (atx-impl/tools/test_nav_summ.write_nav, calendar-day sessions from 2020-01-02, inside TRAIN);
the real nav_summ.py scores them with the argv the cycle's summ step builds.
"""
from __future__ import annotations

import json
import math
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
import cycle_verdict as CV  # noqa: E402
import nav_summ as NS  # noqa: E402
import research_cycle as RC  # noqa: E402
from test_nav_summ import write_nav, write_weights  # noqa: E402
from test_research_cycle import cycle_of, make_root  # noqa: E402

NAV_SUMM = (TOOLS / "nav_summ.py").as_posix()
ROOT_252 = math.sqrt(252)


def unit_normal(t: int, seed: int) -> np.ndarray:
    """t normal draws standardised to mean 0 and sample SD (ddof 1) 1 exactly."""
    z = np.random.default_rng(seed).normal(size=t)
    return (z - z.mean()) / z.std(ddof=1)


def ledger_line(k: int, sr: float, window_id: str | None) -> dict:
    """A construction line as nav_summ writes it, with only the keys the ledger readers use."""
    rec = {"schema": BI.LEDGER_SCHEMA, "kind": "construction", "count": 1, "cell": f"prior/c{k:02d}",
           "window": {"label": "TRAIN", "first_session": "2020-01-02", "last_session": "2023-12-29", "sessions": 1006},
           "s2_net_sr": sr, "trial_id": f"{k:016x}"}
    if window_id is not None:
        rec.update(window_id=window_id, origin="prior")
    return rec


def scoring_root(tmp_path: Path, *, sr_annual: float, t: int, prior_srs: list[float], summ_extra: list[str]):
    """A cycle root whose fit, reference and NAV outputs exist (the cell: t return rows, net Sharpe sr_annual
    exactly), and a trial ledger of construction lines on the current research window with the given S2 net SRs."""
    root, sp = make_root(tmp_path, summ={"script": NAV_SUMM, "dsr_n": "ledger+1", "ledger": "trials.jsonl",
                                         "origin": "prior", "extra": summ_extra}, verdict=True)
    sigma = 0.01
    nets = sigma / ROOT_252 * sr_annual + sigma * unit_normal(t, 7)
    write_nav(root / "out" / "N", list(nets), net_sharpe=sr_annual)
    ref = write_nav(tmp_path / "refnav", list(0.0002 + sigma * unit_normal(t, 8)))
    for f in ref.iterdir():
        shutil.copyfile(f, root / "ref-cell" / f.name)
    (root / "out" / "W").mkdir(parents=True)
    write_weights(root / "out" / "W" / "composition_weights.json", 0.1)
    wid = BI.window_id()
    (root / "trials.jsonl").write_text("".join(json.dumps(ledger_line(k, sr, wid), sort_keys=True) + "\n"
                                               for k, sr in enumerate(prior_srs)), encoding="utf-8")
    return root, sp


def run_summ(root: Path, sp: Path, monkeypatch) -> tuple[list[str], list[dict]]:
    """The summ step's argv, run through the real nav_summ (in-process, cwd = the root); its --json rows."""
    c = cycle_of(root, sp)
    argv = next(s for s in c.steps() if s.phase == "summ").argv
    assert argv[:2] == [sys.executable, NAV_SUMM]
    (root / c.cycle_dir()).mkdir(parents=True, exist_ok=True)
    monkeypatch.chdir(root)
    assert NS.main(argv[2:]) == 0
    return argv, json.loads((root / c.cycle_dir() / CV.SUMM_JSON).read_text(encoding="utf-8"))


def test_verdict_dsr_is_the_ledger_variance_value_not_the_single_cell_lo_value(tmp_path, monkeypatch, capsys):
    """Review C-1's example: normal returns, T 1,006, SR 1.0 annual, N 41. The single-cell Lo (2002) variance gives
    SR0 1.10 annual and DSR .42; the pre-registered cross-trial variance (SD .15 annual over the ledgered cells)
    gives SR0 .33 and DSR .91. The verdict carries the second."""
    prior = [1.15] * 20 + [0.85] * 20            # with this cell's 1.0: 41 SRs, mean 1.0, sample SD .15 exactly
    root, sp = scoring_root(tmp_path, sr_annual=1.0, t=1006, prior_srs=prior, summ_extra=[])
    monkeypatch.setattr(NS, "V8_DRAWS", 99)       # speed only: a v8 spec may not pass --draws (review F-2)
    argv, rows = run_summ(root, sp, monkeypatch)
    capsys.readouterr()
    k = argv.index("--dsr-ledger")
    assert argv[k + 1] == "trials.jsonl" and argv[argv.index("--ledger") + 1] == "trials.jsonl"
    assert argv[argv.index("--protocol") + 1] == "v8" and argv[argv.index("--origin") + 1] == "prior"   # C-2
    assert argv[argv.index("--dsr-n") + 1] == "41"                              # ledger N + 1 (plan time)
    row = next(r for r in rows if Path(r["dir"]).as_posix() == "out/N")
    m = row["net_moments"]
    assert m["sessions"] == 1006 and m["sr_daily"] * ROOT_252 == pytest.approx(1.0, rel=1e-12)
    dl = row["deflated_ledger"]
    variance = (0.15 / ROOT_252) ** 2
    assert dl["n"] == 41 and dl["cells"] == 41 and dl["window_id"] == BI.window_id()
    assert dl["variance_sr"] == pytest.approx(variance, rel=1e-12)
    want = NS.deflated_sharpe(m["sr_daily"], m["sessions"], m["skew"], m["kurtosis"], NS.expected_max_sr(variance, 41))
    assert dl["dsr"] == pytest.approx(want, rel=1e-12)
    assert dl["sr0_annual"] == pytest.approx(0.33, abs=0.005) and dl["dsr"] == pytest.approx(0.91, abs=0.01)
    lo = row["deflated"]                                                         # the old verdict source
    assert lo["n"] == 41 and lo["variance_source"].startswith("Lo (2002)")
    assert lo["sr0_annual"] == pytest.approx(1.10, abs=0.01) and lo["dsr"] == pytest.approx(0.42, abs=0.01)
    c = cycle_of(root, sp)
    blocks = CV.scoring_blocks(c.res, c.cycle_dir(), "out/N")
    assert blocks["dsr"]["cell_count"] == dl["dsr"] and blocks["dsr"]["n"] == 41
    assert blocks["dsr"]["variance_sr"] == dl["variance_sr"] and blocks["dsr"]["variance_cells"] == 41
    assert abs(blocks["dsr"]["cell_count"] - lo["dsr"]) > 0.4                   # the convention decided the gate


def summ_argv(root: Path, sp: Path, **kw) -> list[str]:
    return next(s for s in cycle_of(root, sp, **kw).steps() if s.phase == "summ").argv


def test_v8_summ_step_carries_the_protocol_and_the_origin(tmp_path):
    """Review C-2: every summ step under a v8 spec (a verdict spec, or --protocol v8 in summ.extra) runs nav_summ
    --protocol v8 (seed 20260929, 4,999 draws) and, with a ledger, --origin summ.origin; a v7 spec's argv is
    unchanged."""
    cyc = "build-equity/cycle-synthetic"
    head = [sys.executable, "scripts/summ.py", "--weights", "out/W/composition_weights.json", "--reference", "ref-cell",
            "--dsr-n", "29"]
    root, sp = make_root(tmp_path / "v8", summ={"script": "scripts/summ.py", "dsr_n": 29, "ledger": "L.jsonl",
                                                "origin": "grid"}, verdict=True)
    assert summ_argv(root, sp) == head + ["--protocol", "v8", "--origin", "grid", "--json", f"{cyc}/summ.json",
                                          "--ledger", "L.jsonl", "--ledger-kind", "construction", "--dsr-ledger",
                                          "L.jsonl", "out/N"]
    a2 = ["--protocol", "v8", "--effective-n", "dirs", "--psr", "--pbo"]           # lane A2's base specs
    root, sp = make_root(tmp_path / "a2", summ={"script": "scripts/summ.py", "dsr_n": 29, "ledger": "L.jsonl",
                                                "origin": "prior", "extra": a2}, verdict=True)
    assert summ_argv(root, sp) == head + a2 + ["--origin", "prior", "--json", f"{cyc}/summ.json", "--pbo-json",
                                               f"{cyc}/pbo.json", "--ledger", "L.jsonl", "--ledger-kind",
                                               "construction", "--dsr-ledger", "L.jsonl", "out/N"]
    root, sp = make_root(tmp_path / "extra", summ={"script": "scripts/summ.py", "dsr_n": 29, "extra": a2[:2],
                                                   "origin": "mined"})
    assert summ_argv(root, sp) == head + ["--protocol", "v8", "out/N"]               # no ledger: no --origin
    assert summ_argv(root, sp, ledger="L.jsonl") == head + ["--protocol", "v8", "--origin", "mined", "--ledger",
                                                            "L.jsonl", "--ledger-kind", "construction", "out/N"]
    root, sp = make_root(tmp_path / "v7", summ={"script": "scripts/summ.py", "dsr_n": 29, "ledger": "L.jsonl"})
    assert summ_argv(root, sp) == head + ["--ledger", "L.jsonl", "--ledger-kind", "construction", "out/N"]  # v7
    bare = make_root(tmp_path / "noorigin", summ={"script": "scripts/summ.py", "dsr_n": 29, "ledger": "L.jsonl"},
                     verdict=True)
    with pytest.raises(RC.CycleError, match="set summ.origin") as e:                  # v8 + ledger needs K5
        summ_argv(*bare)
    assert e.value.code == RC.EXIT_USAGE
    spec = json.loads(sp.read_text())
    boot = "pre-registered paired bootstrap"                                          # review F-2
    for summ, verdict, needle in (({"origin": "lucky"}, None, "summ.origin must be one of"),
                                  ({"origin": "prior", "extra": ["--origin", "grid"]}, None, "not both"),
                                  ({"extra": ["--protocol", "v7"]}, True, "--protocol v8"),
                                  ({"extra": ["--draws", "99"]}, True, f"{boot}.*may not set --draws"),
                                  ({"extra": ["--protocol", "v8", "--seed", "7"]}, None, f"{boot}.*--seed"),
                                  ({"extra": ["--block=5"]}, True, f"{boot}.*--block=5")):
        bad = dict(spec, summ=dict(spec["summ"], **summ))
        if verdict:
            bad["verdict"] = verdict
        with pytest.raises(RC.CycleError, match=needle) as e:
            RC.validate_spec(bad)
        assert e.value.code == RC.EXIT_USAGE
    RC.validate_spec(dict(spec, summ=dict(spec["summ"], extra=["--draws", "99", "--seed", "7"])))   # a v7 spec: as before


def test_add_alpha_from_a_v8_parent_inherits_the_v8_protocol(tmp_path):
    """Review C-2: a spec add-alpha derives from a v8 parent keeps --protocol v8 (so nav_summ's pre-registered seed
    20260929 and 4,999 draws), drops the parent's --origin for the new members' class in summ.origin, and its summ
    step ledgers the cell with that origin."""
    import test_research_cycle as T
    root = T.add_alpha_root(tmp_path)
    parent_path = root / "scripts" / "specs" / "v70.json"
    parent = json.loads(parent_path.read_text())
    parent["verdict"] = True
    parent["summ"]["extra"] = ["--protocol", "v8", "--origin", "prior", "--effective-n", "dirs", "--psr", "--pbo"]
    parent_path.write_text(json.dumps(parent))
    RC.validate_spec(parent)
    s = root / "atx-impl" / "strategies"
    v70_ids = [c["id"] for c in json.loads((s / "fund_industry_ic_v70.json").read_text())["candidates"]]
    plan = T.k1_plan(tmp_path, v70_ids + ["ftd_fail"])
    grid = ["build-equity/mega-nav-v5-ew-t.05-d.1-fixed", T.V70_CELL]
    (root / "build-equity" / "trials.jsonl").write_text("".join(json.dumps(ledger_line(k, 1.0, None) | {"cell": c},
                                                                           sort_keys=True) + "\n"
                                                                for k, c in enumerate(grid)), encoding="utf-8")
    add = T.add_argv(root, "ftd_fail")
    add[add.index("--origin") + 1] = "grid"                                          # the new member's K5 class
    assert RC.main(add + ["--plan-json", str(plan)]) == RC.EXIT_OK
    child = RC.load_spec(root / "scripts" / "specs" / "v8" / "lib-v71a.json")
    assert child["verdict"] is True and child["summ"]["origin"] == "grid"
    assert child["summ"]["extra"] == ["--protocol", "v8", "--effective-n", "dirs", "--psr", "--pbo"]
    c = RC.Cycle(child, RC.Resolver(root), capabilities=T.CAPS)
    w_dir, n_out = c.out(child["fit"]["output"], keyed=False), c.out(child["nav"]["output"])
    argv = c.summ_step(w_dir, n_out).argv
    k = argv.index("--dsr-n")
    cyc = c.cycle_dir()
    assert argv[k:] == ["--dsr-n", "3", "--protocol", "v8", "--effective-n", "dirs", "--psr", "--pbo", "--origin", "grid",
                        "--json", f"{cyc}/summ.json", "--pbo-json", f"{cyc}/pbo.json", "--ledger",
                        "build-equity/trials.jsonl", "--ledger-kind", "construction", "--dsr-ledger",
                        "build-equity/trials.jsonl"]
    assert argv[argv.index("--reference") + 2:k] == grid + [n_out]
    assert "--seed" not in argv and "--draws" not in argv                            # nav_summ's v8 defaults apply
    assert (NS.V8_SEED, NS.V8_DRAWS) == (20260929, 4999)
    assert T.RA.wave_origin(["prior", "grid", "prior"]) == "grid" and T.RA.wave_origin(["prior", "mined"]) == "mined"


def test_verdict_refuses_a_dsr_without_the_ledger(tmp_path):
    root, sp = make_root(tmp_path, summ={"script": "scripts/summ.py", "dsr_n": 3, "origin": "prior"}, verdict=True)
    with pytest.raises(RC.CycleError, match="sprint ledger of record") as e:     # no ledger: no verdict summ step
        cycle_of(root, sp).steps()
    assert e.value.code == RC.EXIT_USAGE
    assert "--dsr-ledger" in next(s for s in cycle_of(root, sp, ledger="L.jsonl").steps() if s.phase == "summ").argv
    c = cycle_of(root, sp, ledger="L.jsonl")
    (root / c.cycle_dir()).mkdir(parents=True)
    (root / c.cycle_dir() / CV.SUMM_JSON).write_text(json.dumps([{"dir": "out/N", "deflated": {"n": 3, "dsr": 0.99}}]))
    with pytest.raises(CV.VerdictError, match="never from a cell count"):
        CV.scoring_blocks(c.res, c.cycle_dir(), "out/N")
    with pytest.raises(RC.CycleError, match=r"HARD-STOP \[verdict\]"):
        RC.write_verdict(c, {}, lambda s: None)
