"""Tests of the total-count deflated Sharpe ratio (dsr_total.py; nav_summ --dsr-total / --dsr-hand).

These implement v8x-prereg.md section 4, precondition P5 (Rulings PM7-1, PM7-2, PM7-7). Synthetic data only.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_dsr_total.py

Every expected value is computed in the test from the formula: numpy for the moments and the variance, and
statistics.NormalDist for Phi and PhiInv. The bisection quantile of backtest_integrity is not used, so the tool is
checked against an independent oracle.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
from statistics import NormalDist
import subprocess

import numpy as np
import pytest

import backtest_integrity as BI
import dsr_total as DT
import nav_summ as NS
from test_nav_summ import SCEN, write_nav

ND = NormalDist()
GAMMA = 0.5772156649015329
ROOT = math.sqrt(252)
PRE_X_BLOB = "ef8cbbec97ec0e5d7590b1b7e3c0d327c1b1fab3"   # nav_summ.py at 3c6ae225 (last changed in 0b855971)
REGISTRY_COUNT = 132                                        # the campaign's evaluations (v9-mine-c1's budget)
WINDOW_SRS = {"W0": 0.6, "B0": 0.9, "B1": 1.3, "HF": 1.2, "XF0": 1.25}   # ledgered s2_net_sr of the window cells


# ------------------------------------------------------------------ the oracle
def oracle(x, n: int, variance: float) -> dict:
    """DSR from the formula of v8x-prereg.md section 4, with independent Phi and PhiInv."""
    x = np.asarray(x, dtype=np.float64)
    t = x.size
    e = x - x.mean()
    m2 = float((e ** 2).mean())
    sr = float(x.mean()) / float(x.std(ddof=1))
    g3, g4 = float((e ** 3).mean()) / m2 ** 1.5, float((e ** 4).mean()) / m2 ** 2
    sr0 = math.sqrt(variance) * ((1 - GAMMA) * ND.inv_cdf(1 - 1 / n) + GAMMA * ND.inv_cdf(1 - 1 / (n * math.e)))
    return {"sr0": sr0, "dsr": ND.cdf((sr - sr0) * math.sqrt(t - 1) / math.sqrt(1 - g3 * sr + (g4 - 1) * sr * sr / 4))}


def v_ddof1(srs) -> float:
    return float(np.var(np.asarray(srs) / ROOT, ddof=1))


# ------------------------------------------------------------------ a synthetic ledger of record
def nets_of(seed: int, mu: float, t: int = 300) -> list[float]:
    rng = np.random.default_rng(seed)
    return list(mu + 0.01 * rng.standard_t(5, size=t))      # fat tails: skew and kurtosis enter the denominator


def cell(tmp_path: Path, name: str, seed: int, mu: float = 0.0006) -> Path:
    return write_nav(tmp_path / name, nets_of(seed, mu))


def record(d: Path, sr: float, **kw) -> dict:
    return BI.ledger_record("construction", str(d), d / "summary.json", d / f"daily_{SCEN}.csv", SCEN,
                            NS.net_series(NS.load_daily(d, SCEN)), sr, **kw)


def admission(name: str, origin: str = "prior") -> dict:
    """An admission line as cycle_admission.py writes it (one per listed candidate, count 1)."""
    return {"schema": BI.LEDGER_SCHEMA, "kind": "admission", "count": 1, "candidate": name, "cycle": "x-wave",
            "status": "admitted", "origin": origin, "window_id": BI.window_id(),
            "trial_id": BI.trial_id("admission", hashlib.sha256(name.encode()).hexdigest())}


def protocol_line() -> dict:
    return {"schema": BI.LEDGER_SCHEMA, "kind": "protocol", "count": 0, "window_id": BI.window_id(),
            "date": "2026-09-29", "trial_id": "0123456789abcdef"}


def campaign() -> dict:
    return BI.campaign_line("v9-mine-c1", "build-equity/mine-v9-c1/registry.atxtrg", "ab" * 32, REGISTRY_COUNT,
                            registry_total=REGISTRY_COUNT, registry_bytes=4096, budget=REGISTRY_COUNT,
                            recipe_sha256="cd" * 32, confirm={"begin": "2023-01-01", "end": "2024-01-01"},
                            research_window_id=BI.window_id(), date="2026-10-10")


@pytest.fixture()
def book(tmp_path):
    """The ledger of record of an X program, in ledger order:
    - 3 legacy construction cells (v7 layout, unchained): 3 trials, not in V;
    - a protocol line: 0;
    - a window re-run of A0: 0 trials, in V;
    - B0 and B1: 2 trials, in V;
    - BAD, ledgered invalid at once: 0, not in V;
    - 7 hand-written admission lines: 7;
    - HF, the last hand-written book: 1, in V;
    - the campaign line: 0 in trial_counts, registry count 132;
    - 3 mined admission lines: 3;
    - XF0, the book of the X gate: 1, in V.
    Totals: trial_counts 17, M 132, N_tot 149, N_c 7, five cells in V. HF's prefix: 16 lines, N_tot 13, four cells in V.
    """
    wid = BI.window_id()
    names = ("A0", "A1", "A2", "W0", "B0", "B1", "BAD", "HF", "XF0", "NEW")
    dirs = {k: cell(tmp_path, k, seed) for seed, k in enumerate(names)}
    ledger = tmp_path / "trials.jsonl"
    legacy = [record(dirs[k], sr) for k, sr in (("A0", 0.5), ("A1", 1.0), ("A2", 1.5))]
    BI.ledger_append(ledger, legacy)
    lines = [protocol_line(),
             record(dirs["W0"], WINDOW_SRS["W0"], research_window_id=wid, rerun_of=legacy[0]["trial_id"],
                    rerun_basis="window"),
             record(dirs["B0"], WINDOW_SRS["B0"], research_window_id=wid, origin="prior"),
             record(dirs["B1"], WINDOW_SRS["B1"], research_window_id=wid, origin="prior"),
             record(dirs["BAD"], 5.0, research_window_id=wid, origin="prior", defect="cost table misread"),
             *[admission(f"hand{i}") for i in range(7)],
             record(dirs["HF"], WINDOW_SRS["HF"], research_window_id=wid, origin="prior"),
             campaign(),
             *[admission(f"mined{i}", "mined") for i in range(3)],
             record(dirs["XF0"], WINDOW_SRS["XF0"], research_window_id=wid, origin="mined")]
    BI.ledger_append(ledger, lines, chain=True)
    return {"dirs": dirs, "ledger": ledger, "tmp": tmp_path}


def run_json(tmp_path: Path, argv: list[str], capsys) -> tuple[list[dict], str]:
    out = tmp_path / "out.json"
    assert NS.main(argv + ["--json", str(out)]) == 0
    return json.loads(out.read_text(encoding="utf-8")), capsys.readouterr().out


# ------------------------------------------------------------------ closed form
def test_total_count_and_dsr_are_the_closed_form(book, capsys):
    d, ledger = book["dirs"], book["ledger"]
    records = BI.ledger_read(ledger)
    assert BI.trial_counts(records) == [1, 1, 1, 0, 0, 1, 1, 0] + [1] * 7 + [1, 0] + [1] * 3 + [1]
    rows, out = run_json(book["tmp"], [str(d["XF0"]), str(d["NEW"]), "--dsr-total", str(ledger)], capsys)
    v = v_ddof1(list(WINDOW_SRS.values()))
    xf0, new = rows[0]["deflated_total"], rows[1]["deflated_total"]
    assert xf0["parts"] == {"by_kind": {"admission": 10, "construction": 7}, "ledger_trials": 17,
                            "campaign_registry": REGISTRY_COUNT, "n_tot": 149}
    assert xf0["in_ledger"] is True and new["in_ledger"] is False
    assert (xf0["tot"]["n"], xf0["v8"]["n"]) == (149, 7)          # the book is ledgered: no + 1
    assert (new["tot"]["n"], new["v8"]["n"]) == (150, 8)          # not yet ledgered: + 1 on both counts
    assert xf0["cells_in_v"] == 5 and xf0["variance_sr"] == pytest.approx(v, rel=1e-12)
    for key, nets in (("XF0", nets_of(8, 0.0006)), ("NEW", nets_of(9, 0.0006))):
        row = xf0 if key == "XF0" else new
        for which in ("tot", "v8"):
            want = oracle(nets, row[which]["n"], v)
            assert row[which]["sr0_daily"] == pytest.approx(want["sr0"], rel=1e-9)
            assert row[which]["sr0_annual"] == pytest.approx(want["sr0"] * ROOT, rel=1e-9)
            assert row[which]["dsr"] == pytest.approx(want["dsr"], rel=1e-9)
    assert xf0["tot"]["dsr"] < xf0["v8"]["dsr"]                    # the larger count deflates more
    head = BI.ledger_head(ledger)
    assert xf0["chain_head"] == head and xf0["lines"] == len(records) == 21
    assert (f"== total-count DSR {d['XF0']} (ledger {ledger}, 21 lines, chain head {head[:16]})") in out
    assert (f"   DSR_tot (N_tot=149): DSR {xf0['tot']['dsr']:.4f} vs SR0 {xf0['tot']['sr0_annual']:.3f} ann | "
            f"N_tot = admission 10 + construction 7 + campaign registry 132 | V[SR] {v:.3e} from 5 cells ledgered on "
            f"{BI.window_id()} | ") in out
    assert "   DSR_tot (N_tot=150): " in out and "+ campaign registry 132 + 1 (this cell, not yet ledgered) |" in out
    assert f"   DSR_v8 (N_c=7, v8 rule 3 as --dsr-ledger): DSR {xf0['v8']['dsr']:.4f} vs SR0 " in out


def test_the_v8_count_value_is_the_dsr_ledger_value(book, capsys):
    d, ledger = book["dirs"], book["ledger"]
    rows, _ = run_json(book["tmp"], [str(d["XF0"]), str(d["NEW"]), "--dsr-total", str(ledger), "--dsr-ledger",
                                     str(ledger)], capsys)
    for r in rows:
        assert r["deflated_total"]["v8"]["n"] == r["deflated_ledger"]["n"]
        assert r["deflated_total"]["v8"]["dsr"] == pytest.approx(r["deflated_ledger"]["dsr"], rel=1e-12)
        assert r["deflated_total"]["variance_sr"] == r["deflated_ledger"]["variance_sr"]


def test_a_campaign_line_changes_n_tot_and_nothing_else(book, capsys):
    d, ledger = book["dirs"], book["ledger"]
    texts = ledger.read_text(encoding="utf-8").splitlines()
    k = next(i for i, line in enumerate(texts) if json.loads(line)["kind"] == BI.MINING_CAMPAIGN)
    without = book["tmp"] / "no-campaign.jsonl"                  # the same program without its campaign line:
    BI.ledger_append(without, [json.loads(line) for line in texts[:3]])     # rebuilt so the chain verifies
    BI.ledger_append(without, [{x: y for x, y in json.loads(line).items() if x != "prev_sha256"}
                               for i, line in enumerate(texts[3:], 3) if i != k], chain=True)
    with_c, _ = run_json(book["tmp"], [str(d["XF0"]), "--dsr-total", str(ledger)], capsys)
    no_c, _ = run_json(book["tmp"], [str(d["XF0"]), "--dsr-total", str(without)], capsys)
    a, b = with_c[0]["deflated_total"], no_c[0]["deflated_total"]
    assert a["tot"]["n"] - b["tot"]["n"] == REGISTRY_COUNT and a["parts"]["campaign_registry"] == REGISTRY_COUNT
    assert b["parts"]["campaign_registry"] == 0 and a["parts"]["by_kind"] == b["parts"]["by_kind"]
    assert a["v8"] == b["v8"] and a["variance_sr"] == b["variance_sr"] and a["cells_in_v"] == b["cells_in_v"]
    assert a["tot"]["dsr"] < b["tot"]["dsr"]
    nets = nets_of(8, 0.0006)
    assert b["tot"]["dsr"] == pytest.approx(oracle(nets, 149 - REGISTRY_COUNT, a["variance_sr"])["dsr"], rel=1e-9)


def test_the_variance_is_ddof_1_over_window_cells_only(book):
    records = BI.ledger_read(book["ledger"])
    moments = NS.net_moments(np.array(nets_of(8, 0.0006)))
    rows = DT.ledger_rows(moments, records, True, BI.window_id())
    srs = np.array(list(WINDOW_SRS.values())) / ROOT
    ddof0 = float(np.var(srs, ddof=0))
    assert rows["variance_sr"] == pytest.approx(float(np.var(srs, ddof=1)), rel=1e-12)
    assert abs(rows["variance_sr"] - ddof0) > 0.2 * ddof0                 # ddof 1 is 5/4 of ddof 0 at five cells
    assert rows["tot"]["dsr"] != pytest.approx(DT.dsr_at(moments, 149, ddof0)["dsr"], rel=1e-6)
    # legacy cells (no window_id), the invalid cell and the admission / campaign lines never enter V
    assert rows["cells_in_v"] == 5 and "ddof 1" in rows["variance_source"]
    # below two window cells V is undefined: the DSR is None (printed "na"), never a silent fallback
    short = DT.ledger_rows(moments, records[:5], True, BI.window_id())
    assert short["variance_sr"] is None and short["tot"]["dsr"] is None and short["tot"]["n"] == 3


def test_dsr_hand_reads_the_prefix_ending_at_the_books_own_line(book, capsys):
    d, ledger = book["dirs"], book["ledger"]
    rows, out = run_json(book["tmp"], [str(d["XF0"]), "--dsr-total", str(ledger), "--dsr-hand", str(d["HF"]),
                                       "--dsr-hand", str(d["B0"])], capsys)
    hf, b0 = rows[0]["deflated_total"]["hand"]
    texts = [line for line in ledger.read_text(encoding="utf-8").splitlines() if line.strip()]
    assert (hf["prefix_lines"], b0["prefix_lines"]) == (16, 6)
    assert hf["parts"] == {"by_kind": {"admission": 7, "construction": 6}, "ledger_trials": 13,
                           "campaign_registry": 0, "n_tot": 13}           # the campaign line comes after H-F
    assert hf["tot"]["n"] == 13 and hf["in_ledger"] is True and hf["cells_in_v"] == 4
    v_hand = v_ddof1([WINDOW_SRS[k] for k in ("W0", "B0", "B1", "HF")])
    assert hf["variance_sr"] == pytest.approx(v_hand, rel=1e-12)
    assert hf["tot"]["dsr"] == pytest.approx(oracle(nets_of(7, 0.0006), 13, v_hand)["dsr"], rel=1e-9)
    assert hf["prefix_chain_head"] == BI.chain_head(texts[:16])
    cut = book["tmp"] / "prefix.jsonl"                             # the head a ledger of exactly those lines has
    cut.write_text("\n".join(texts[:16]) + "\n", encoding="utf-8")
    assert BI.ledger_head(cut) == hf["prefix_chain_head"]
    assert b0["tot"]["n"] == 4 and b0["cells_in_v"] == 2          # B0's own ledger state (V8-F's use of the rule)
    assert hf["trial_id"] == BI.trial_id("construction", BI.sha256_file(d["HF"] / f"daily_{SCEN}.csv"))
    assert (f"== DSR_hand {d['HF']}: ledger prefix of 16 lines ending at its construction line {hf['trial_id']} "
            f"(chain head {hf['prefix_chain_head'][:16]})") in out
    assert "   DSR_hand (N_tot=13): DSR " in out
    assert "N_tot = admission 7 + construction 6 + campaign registry 0 |" in out


# ------------------------------------------------------------------ refusals
def test_refusals(book, capsys):
    d, ledger, tmp = book["dirs"], book["ledger"], book["tmp"]
    with pytest.raises(SystemExit, match="does not exist"):        # a missing ledger: never read as N 0
        NS.main([str(d["XF0"]), "--dsr-total", str(tmp / "absent.jsonl")])
    with pytest.raises(SystemExit, match="no summary.json"):       # a missing --dsr-hand source file
        NS.main([str(d["XF0"]), "--dsr-total", str(ledger), "--dsr-hand", str(tmp / "nowhere")])
    (d["NEW"] / f"daily_{SCEN}.csv").rename(d["NEW"] / "moved.csv")
    with pytest.raises(SystemExit, match=re.escape(f"no daily_{SCEN}.csv")):
        NS.main([str(d["XF0"]), "--dsr-total", str(ledger), "--dsr-hand", str(d["NEW"])])
    (d["NEW"] / "moved.csv").rename(d["NEW"] / f"daily_{SCEN}.csv")
    with pytest.raises(SystemExit, match="has no construction line"):   # a book never ledgered has no ledger state
        NS.main([str(d["XF0"]), "--dsr-total", str(ledger), "--dsr-hand", str(d["NEW"])])
    broken = tmp / "broken.jsonl"                                   # an edited ledger: the hash chain breaks
    texts = ledger.read_text(encoding="utf-8").splitlines()
    broken.write_text("\n".join(texts[:5] + [texts[5].replace("prior", "grid")] + texts[6:]) + "\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="hash chain broken"):
        NS.main([str(d["XF0"]), "--dsr-total", str(broken)])
    for argv in ([str(d["XF0"]), "--dsr-hand", str(d["HF"])],                         # --dsr-hand alone
                 ["--dsr-total", str(ledger), "--ledger-n", str(ledger)],             # no NAV dir
                 [str(d["XF0"]), "--dsr-total", str(ledger), "--pool", str(d["HF"])]):  # an era pool
        with pytest.raises(SystemExit):
            NS.main(argv)
    capsys.readouterr()


# ------------------------------------------------------------------ identity: flag absent
def load_blob(tmp_path: Path, blob: str, name: str):
    try:
        text = subprocess.run(["git", "cat-file", "-p", blob], capture_output=True, check=True,
                              cwd=Path(NS.__file__).resolve().parent).stdout
    except (OSError, subprocess.CalledProcessError):
        pytest.skip(f"git history with the nav_summ blob {blob[:8]} is unavailable")
    path = tmp_path / "old" / f"{name}.py"
    path.parent.mkdir(exist_ok=True)
    path.write_bytes(text)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def without_run(doc):
    """A --json / --bundle-json document without nav_summ_run (the script's own SHA-256 and git head)."""
    if isinstance(doc, list):
        return [{k: v for k, v in r.items() if k != "nav_summ_run"} for r in doc]
    return {k: v for k, v in doc.items() if k != "nav_summ_run"}


def test_flag_absent_is_byte_identical_to_the_pre_x_nav_summ(book, capsys):
    """Against nav_summ.py at the lane's base (blob ef8cbbec), for the same inputs and argv without --dsr-total,
    stdout, stderr and every JSON field except nav_summ_run are byte-identical."""
    pre = load_blob(book["tmp"], PRE_X_BLOB, "nav_summ_pre_x")
    d, ledger, tmp = book["dirs"], str(book["ledger"]), book["tmp"]
    cells = [str(d[k]) for k in ("B0", "B1", "HF", "XF0")]
    argvs = [(cells + ["--reference", cells[0], "--draws", "200", "--json"], "--json"),
             (cells + ["--protocol", "v8", "--draws", "200", "--dsr-ledger", ledger, "--ledger-n", ledger, "--psr",
                       "--effective-n", "dirs", "--pbo", "--json"], "--json"),
             (cells[:2] + ["--effective-n", ledger, "--dsr-n", "4", "--json"], "--json"),
             (["--bundle", cells[0], cells[3], "--protocol", "v8", "--draws", "200", "--bundle-json"], "--bundle-json")]
    for k, (argv, flag) in enumerate(argvs):
        seen = {}
        for tag, module in (("new", NS), ("old", pre)):
            out = tmp / f"id{k}-{tag}.json"
            assert module.main(argv + [str(out)]) == 0
            got = capsys.readouterr()
            seen[tag] = (got.out, got.err, without_run(json.loads(out.read_text(encoding="utf-8"))))
        assert seen["new"][0] == seen["old"][0], f"stdout differs for argv {k}"
        assert seen["new"][1] == seen["old"][1], f"stderr differs for argv {k}"
        assert json.dumps(seen["new"][2], sort_keys=True) == json.dumps(seen["old"][2], sort_keys=True), flag


def test_the_flags_only_add_lines_and_one_json_key(book, capsys):
    d, ledger = book["dirs"], str(book["ledger"])
    base = [str(d["B0"]), str(d["XF0"]), "--protocol", "v8", "--draws", "200", "--dsr-ledger", ledger]
    plain, out_plain = run_json(book["tmp"], base, capsys)
    extra, out_extra = run_json(book["tmp"], base + ["--dsr-total", ledger, "--dsr-hand", str(d["HF"])], capsys)
    added = ("== total-count DSR ", "   DSR_tot ", "   DSR_v8 ", "== DSR_hand ", "   DSR_hand ")
    kept = [line for line in out_extra.splitlines() if not line.startswith(added)]
    assert kept == out_plain.splitlines() and len(out_extra.splitlines()) - len(kept) == 2 * 3 + 2
    assert [set(a) - set(b) for a, b in zip(extra, plain)] == [{"deflated_total"}] * 2
    for a, b in zip(without_run(extra), without_run(plain)):
        a.pop("deflated_total")
        assert json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True)
