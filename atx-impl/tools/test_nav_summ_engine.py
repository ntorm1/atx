"""nav_summ.py's statistics of record from the engine verb atx-research-eval (platform P9 B2, K-P9-5), after the
deletion of the Python statistics.

  - the tie (review B2 S6): nav_summ's --json / --pbo-json / --bundle-json blocks equal the retired Python statistics
    (fixtures/eval_tie/frozen_statistics.py, the deleted code verbatim) on the same net series: floats within 1e-12,
    paired floats within 1e-12 / (1 - rho) with equal counts, a pair with 1 - rho < 1e-6 refused as degenerate
    (ruling B2-R1);
  - the verb (ruling B2-R3): $ATX_EQUITY_BIN/atx-research-eval.exe when ATX_EQUITY_BIN is set, refused there without
    it (no fallback), else build-equity/bin of this checkout, or --engine-stats EXE; every run that used it names it
    (engine_exe, engine_exe_sha256 in nav_summ_run); a run that computes no statistic opens none (review B2 S8).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_nav_summ_engine.py

Synthetic NAV dirs only (test_nav_summ.write_nav). Without the built verb the tests fail (nav_summ refuses), as every
nav_summ suite does after the deletion; they never skip.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import math
import sys
from pathlib import Path

import numpy as np
import pytest

import dsr_total as DT
import nav_summ as NS
from test_nav_summ import write_nav

BI = NS.BI
EXE = NS.default_engine_exe()
REPO = Path(__file__).resolve().parents[2]
FROZEN = REPO / "atx-engine" / "tests" / "fixtures" / "eval_tie" / "frozen_statistics.py"
EDGES = REPO / "atx-engine" / "tests" / "fixtures" / "research_eval_edges"
COUNTS = ("p_value", "p_one_sided", "valid_draws", "cbb_valid_draws")


def frozen():
    """The retired Python statistics (loaded by path under a private name: no import of a deleted module)."""
    spec = importlib.util.spec_from_file_location("b2_frozen_statistics_engine_test", FROZEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


FS = frozen()


def cells(tmp_path: Path, k: int, t: int = 260, seed: int = 3) -> list[Path]:
    rng = np.random.default_rng(seed)
    common = 0.006 * rng.standard_normal(t)
    out = []
    for j in range(k):
        nets = 0.0003 * (j + 1) + common * (0.5 + 0.1 * j) + 0.004 * rng.standard_t(5, size=t)
        out.append(write_nav(tmp_path / f"cell{j}", list(nets)))
    return out


def run(argv: list[str], capsys) -> tuple[str, str]:
    assert NS.main(argv) == 0
    out = capsys.readouterr()
    return out.out, out.err


def nets_of(d: Path) -> dict:
    """A NAV dir's net series as nav_summ reads it (session_ns -> net, file order)."""
    return NS.net_series(NS.load_daily(d, NS.scenario_of(NS.load_summary(d))["scenario"]))


def values(nets: dict) -> np.ndarray:
    return np.array(list(nets.values()), dtype=np.float64)


def plain(x):
    """JSON's view of a Python result: numpy scalars and arrays to Python, tuples to lists."""
    if isinstance(x, dict):
        return {k: plain(v) for k, v in x.items()}
    if isinstance(x, (list, tuple, np.ndarray)):
        return [plain(v) for v in x]
    if isinstance(x, np.floating):
        return float(x)
    if isinstance(x, np.integer):
        return int(x)
    return x


def tie(want, got, path: str = "", rel: float = 1e-12) -> list[str]:
    """Differences of ``got`` (nav_summ's JSON) from ``want`` (the frozen Python): every key of ``want`` present
    (extra keys allowed), numbers within ``rel`` (NaN == NaN), the rest equal. A "paired" block ties within
    1e-12 / (1 - rho) of the Python's rho and its p-values and counts exactly (ruling B2-R1 (b))."""
    want = plain(want)
    if isinstance(want, dict):
        if not isinstance(got, dict):
            return [f"{path}: not an object"]
        out = []
        for k, w in want.items():
            if k not in got:
                out.append(f"{path}.{k}: missing")
                continue
            sub = rel
            if k == "paired" and isinstance(w, dict) and isinstance(w.get("rho"), float) and w["rho"] < 1.0:
                sub = 1e-12 / (1.0 - w["rho"])
            if k in COUNTS:
                sub = 0.0
            out += tie(w, got[k], f"{path}.{k}", sub)
        return out
    if isinstance(want, list):
        if not isinstance(got, list) or len(got) != len(want):
            return [f"{path}: length"]
        return [p for i, (w, g) in enumerate(zip(want, got)) for p in tie(w, g, f"{path}[{i}]", rel)]
    number = (int, float)
    if isinstance(want, number) and isinstance(got, number) and not isinstance(want, bool):
        a, b = float(want), float(got)
        if a == b or (math.isnan(a) and math.isnan(b)) or (rel > 0 and math.isclose(a, b, rel_tol=rel, abs_tol=1e-300)):
            return []
        return [f"{path}: Python {want!r} engine {got!r}"]
    return [] if want == got else [f"{path}: Python {want!r} engine {got!r}"]


def test_pbo_max_splits_has_no_engine_twin(tmp_path):
    d = cells(tmp_path, 1)[0]
    with pytest.raises(SystemExit):
        NS.main([str(d), "--pbo", "--pbo-max-splits", "10", "--engine-stats", str(tmp_path / "none.exe")])


def test_a_missing_executable_is_refused(tmp_path):
    d = cells(tmp_path, 1)[0]
    with pytest.raises(SystemExit, match="executable"):
        NS.main([str(d), "--engine-stats", str(tmp_path / "none.exe")])


def test_a_bin_without_the_verb_fails_closed(tmp_path, monkeypatch, capsys):
    """Ruling B2-R3: ATX_EQUITY_BIN names a bin without atx-research-eval.exe: refused, naming that bin's path; never
    the checkout's build-equity/bin (whose Debug verb may be stale). A run that computes no statistic (--ledger-n
    alone, review B2 S8) needs no verb."""
    d = cells(tmp_path, 1)[0]
    empty = tmp_path / "canary-bin"
    empty.mkdir()
    monkeypatch.setenv("ATX_EQUITY_BIN", str(empty))
    assert NS.default_engine_exe() == empty / "atx-research-eval.exe"
    with pytest.raises(SystemExit, match="no engine statistics executable") as e:
        NS.main([str(d)])
    assert str((empty / "atx-research-eval.exe").resolve()) in str(e.value)
    assert str(NS.ENGINE_EXE) not in str(e.value)
    ledger = tmp_path / "fixture-ledger.jsonl"            # never the ledger of record (review B2 S10)
    monkeypatch.setenv("ATX_EQUITY_BIN", str(EXE.parent))
    run([str(d), "--ledger", str(ledger)], capsys)        # a ledger to count (with the verb)
    monkeypatch.setenv("ATX_EQUITY_BIN", str(empty))
    out, _ = run(["--ledger-n", str(ledger)], capsys)
    assert f"Appendix A (trial ledger {ledger}): 1 trials in 1 ledger lines" in out


def test_every_statistic_ties_the_frozen_python(tmp_path, capsys):
    """Review B2 S6: one v8 run with the paired dSR, PSR, the DSR rows, effective N, PBO, the ledger DSR, DSR_tot and
    DSR_hand; every block of --json / --pbo-json against the retired Python on the same series."""
    dirs = cells(tmp_path, 6)
    ledger = tmp_path / "fixture-ledger.jsonl"
    run([str(d) for d in dirs[:4]] + ["--ledger", str(ledger), "--protocol", "v8", "--origin", "prior"], capsys)
    listed = dirs[1:]
    out, _ = run([str(d) for d in listed] + ["--reference", str(dirs[0]), "--protocol", "v8", "--draws", "499",
                                             "--psr", "--effective-n", "dirs", "--pbo", "--pbo-blocks", "8",
                                             "--dsr-ledger", str(ledger), "--dsr-total", str(ledger),
                                             "--dsr-hand", str(dirs[2]), "--json", str(tmp_path / "en.json"),
                                             "--pbo-json", str(tmp_path / "en_pbo.json")], capsys)
    assert "DSR_hand" in out and "CSCV PBO" in out and "effective N" in out and "paired vs" in out
    rows = json.loads((tmp_path / "en.json").read_text(encoding="utf-8"))
    nets = [nets_of(d) for d in listed]
    ref = nets_of(dirs[0])
    moments = [FS.net_moments(values(n)) for n in nets]
    records, wid = BI.ledger_read(ledger), BI.window_id()
    present = {r.get("trial_id") for r in records if r.get("kind") == "construction"}
    _, mat = BI.align_many(nets)
    eff = FS.effective_trials(mat, [str(d) for d in listed], n_init=10, seed=NS.DEFAULT_SEED)
    bad = []
    for d, n, m, row, q in zip(listed, nets, moments, rows, FS.dsr_rows(moments, NS.DEFAULT_DSR_N)):
        at = d.name
        a, b = NS.align(n, ref)
        bad += tie(m, row["net_moments"], f"{at}.net_moments")
        bad += tie(q, row["deflated"], f"{at}.deflated")
        bad += tie(FS.psr_report(values(n), (0.0, 0.5), 0.05), row["psr"], f"{at}.psr")
        bad += tie({"paired": FS.paired_stats(a, b, 499, NS.DEFAULT_BLOCK, NS.V8_SEED)}, row, at)
        bad += tie(dict(FS.effective_n_dsr(m, eff), clusters=eff["clusters"], source="dirs"),
                   row["deflated_effective_n"], f"{at}.deflated_effective_n")
        bad += tie(FS.lo_null_dsr(m, NS.DEFAULT_DSR_N), row["deflated_lo_null"], f"{at}.deflated_lo_null")
        tid = BI.trial_id("construction", BI.sha256_file(d / f"daily_{row['scenario']}.csv"))
        lr = FS.ledger_rows(m, records, tid in present, wid)
        bad += tie(lr, row["deflated_total"], f"{at}.deflated_total")
        v = BI.dsr_variance(records, wid)        # the ledger DSR (nav_summ.ledger_dsr): N = ledger_n, V = window
        house = {"n": BI.ledger_n(records, tid in present), "variance_sr": v["variance_sr"], "cells": v["cells"]}
        house.update({k: lr["v8"][k] for k in ("dsr", "sr0_daily", "sr0_annual")})
        legacy = FS.dsr_at(m, house["n"], v["legacy_variance_sr"])
        house.update(legacy_dsr=legacy["dsr"], legacy_sr0_annual=legacy["sr0_annual"])
        bad += tie(house, row["deflated_ledger"], f"{at}.deflated_ledger")
    hand_tid = BI.trial_id("construction", BI.sha256_file(dirs[2] / f"daily_{rows[0]['scenario']}.csv"))
    k = DT.prefix_length(records, hand_tid)
    hand = dict(FS.ledger_rows(FS.net_moments(values(nets_of(dirs[2]))), records[:k], True, wid), trial_id=hand_tid,
                prefix_lines=k, prefix_chain_head=BI.chain_head(DT.read_ledger(ledger)["lines"][:k]))
    bad += tie(hand, rows[0]["deflated_total"]["hand"][0], "hand")
    pbo = FS.cscv_pbo(mat, blocks=8)
    bad += tie(dict(pbo, logits=list(pbo["logits"])), json.loads((tmp_path / "en_pbo.json").read_text()), "pbo")
    assert bad == []
    assert all(r["paired"]["rho"] < 0.9 for r in rows)    # far pairs: the plain 1e-12 tolerance applies


def test_a_bundle_ties_the_frozen_python(tmp_path, capsys):
    dirs = cells(tmp_path, 2, seed=11)
    out, _ = run([str(dirs[1]), "--protocol", "v8", "--draws", "299", "--bundle", str(dirs[0]), str(dirs[1]),
                  "--bundle-json", str(tmp_path / "b.json")], capsys)
    assert "verdict" in out and "Lo (2002)" in out
    doc = json.loads((tmp_path / "b.json").read_text(encoding="utf-8"))
    a, b = NS.align(nets_of(dirs[1]), nets_of(dirs[0]))
    assert tie({"paired": FS.paired_stats(a, b, 299, NS.DEFAULT_BLOCK, NS.V8_SEED, one_sided=True)}, doc) == []


def test_a_dir_paired_with_itself(tmp_path, capsys):
    """Rulings B2-6 and B2-R1 (c): a dir that is its own reference is a degenerate pair (1 - rho = 0 < 1e-6). The
    engine refuses its Memmel SE, t and the LW CI and p (printed "na", null in --json, with a degenerate line); dSR is
    exactly 0. The retired Python printed numpy's corrcoef residue (1 - rho ~ 2e-16, SE ~ 2e-8) instead."""
    d = cells(tmp_path, 1, seed=13)[0]
    out, _ = run([str(d), "--reference", str(d), "--draws", "99", "--json", str(tmp_path / "s.json")], capsys)
    p = json.loads((tmp_path / "s.json").read_text(encoding="utf-8"))[0]["paired"]
    assert p["dsr"] == 0.0 and p["sr"] == p["sr_reference"] and 1.0 - p["rho"] <= 4e-16
    assert p["degenerate"] is True and p["one_minus_rho"] == 0.0
    assert p["memmel_se"] is None and p["t"] is None and p["lw"]["se"] is None and p["lw"]["p_value"] is None
    assert p["lw"]["ci95"] == [None, None] and p["lw"]["valid_draws"] == 0
    assert "| Memmel SE na t na |" in out and "] p na (dSR_pop" in out
    assert ("   paired: degenerate pair (1 - rho 0.0e+00 < 1e-06): Memmel SE, t and the LW CI and p are refused "
            "(ruling B2-R1)") in out


def edge_cases() -> list[dict]:
    return json.loads((EDGES / "expected.json").read_text(encoding="utf-8"))["paired"]["cases"]


def edge_dirs(tmp_path: Path, case: dict) -> tuple[Path, Path]:
    """The edge fixture's reference and the case's cell as NAV dirs (the fixture's exact generator)."""
    sys.path.insert(0, str(EDGES))
    try:
        import generate_research_eval_edges as G
    finally:
        sys.path.remove(str(EDGES))
    ref = write_nav(tmp_path / f"{case['name']}-ref", list(G.reference()))
    cell = write_nav(tmp_path / f"{case['name']}-cell", list(G.cell(case["eps"], case["shift"])))
    return cell, ref


@pytest.mark.parametrize("case", [c for c in edge_cases() if c["name"] != "self"], ids=lambda c: c["name"])
def test_near_identical_pairs_follow_the_conditioning_rule(tmp_path, capsys, case):
    """Ruling B2-R1 on the edge fixture's pairs (1 - rho 5e-4 .. 6e-13) against the retired Python on the same series:
    with 1 - rho >= 1e-6 nav_summ's --json paired floats are within 1e-12 / (1 - rho), its counts equal; below 1e-6
    the pair is refused (SE / t / p null, "na" and one degenerate line) where the Python gave numbers. (The printed
    lines were byte-equal to the Python's at the identity merge point, before the deletion.)"""
    cell, ref = edge_dirs(tmp_path, case)
    out, _ = run([str(cell), "--reference", str(ref), "--draws", "999", "--seed", "20260929", "--json",
                  str(tmp_path / "en.json")], capsys)
    en = json.loads((tmp_path / "en.json").read_text(encoding="utf-8"))[0]["paired"]
    a, b = NS.align(nets_of(cell), nets_of(ref))
    py = FS.paired_stats(a, b, 999, NS.DEFAULT_BLOCK, 20260929)
    if not case["degenerate"]:
        assert tie({"paired": py}, {"paired": en}) == []
        assert "degenerate" not in en and "paired: degenerate pair" not in out
        return
    assert en["degenerate"] is True and en["one_minus_rho"] < 1e-6
    assert en["memmel_se"] is None and en["t"] is None and en["lw"]["p_value"] is None
    assert isinstance(py["memmel_se"], float) and py["lw"]["p_value"] is not None
    assert "| Memmel SE na t na |" in out and "paired: degenerate pair (1 - rho " in out
    kept = {k: py[k] for k in ("sessions", "sr", "sr_reference", "cbb_valid_draws", "draws", "block", "seed")}
    assert tie(kept, en) == []


def test_every_run_names_its_verb(tmp_path, capsys):
    """Ruling B2-R3: every nav_summ_run (--json, --pbo-json, --bundle-json) records the verb's path and the SHA-256 of
    its bytes: the default verb, or the one --engine-stats names."""
    dirs = cells(tmp_path, 4, seed=21)
    want = {"engine_exe": str(EXE.resolve()), "engine_exe_sha256": hashlib.sha256(EXE.read_bytes()).hexdigest()}
    run([str(d) for d in dirs] + ["--pbo", "--pbo-blocks", "4", "--json", str(tmp_path / "en.json"), "--pbo-json",
                                  str(tmp_path / "en_pbo.json"), "--engine-stats", str(EXE)], capsys)
    run([str(dirs[1]), "--bundle", str(dirs[0]), str(dirs[1]), "--draws", "99", "--bundle-json",
         str(tmp_path / "en_bundle.json")], capsys)
    run([str(dirs[0]), "--json", str(tmp_path / "default.json")], capsys)
    for name in ("en.json", "en_pbo.json", "en_bundle.json", "default.json"):
        doc = json.loads((tmp_path / name).read_text(encoding="utf-8"))
        run_doc = (doc[0] if isinstance(doc, list) else doc)["nav_summ_run"]
        assert {k: run_doc.get(k) for k in want} == want, name
