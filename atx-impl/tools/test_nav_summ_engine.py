"""nav_summ.py --engine-stats (platform P9 B2, K-P9-5): the engine verb atx-research-eval in place of the Python
statistics of record prints the same lines and writes the same JSON keys, values within 1e-12 (the P12 tolerance);
the paired floats within 1e-12 / (1 - rho) and a pair with 1 - rho < 1e-6 refused as degenerate (ruling B2-R1).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_nav_summ_engine.py

Synthetic NAV dirs only (test_nav_summ.write_nav). The verb is the built atx-research-eval.exe: ATX_EQUITY_BIN, else
build-equity/bin of this checkout; without it the comparisons skip (the argument checks still run). With
ATX_ENGINE_STATS_REQUIRED=1 a missing executable fails instead.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import sys
from pathlib import Path

import numpy as np
import pytest

import nav_summ as NS
from test_nav_summ import write_nav

BIN = Path(os.environ.get("ATX_EQUITY_BIN") or NS.ENGINE_EXE.parent)
EXE = BIN / "atx-research-eval.exe"


def need_exe():
    if not EXE.is_file():
        if os.environ.get("ATX_ENGINE_STATS_REQUIRED") == "1":
            pytest.fail(f"no {EXE}")
        pytest.skip(f"no {EXE} (build atx-research-eval)")


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


def close(a, b, path="", rel=1e-12) -> list[str]:
    """Differences of two JSON values: equal structure, ints and strings; floats within ``rel`` (NaN == NaN). A
    "paired" block is compared within 1e-12 / (1 - rho) of the first value's rho (ruling B2-R1 (b)), its p-values and
    counts exactly (decision counts are equal while 1 - rho >= 1e-6)."""
    if isinstance(a, dict) and isinstance(b, dict):
        if set(a) != set(b):
            return [f"{path}: keys {sorted(set(a) ^ set(b))}"]
        out = []
        for k in a:
            sub = rel
            if k == "paired" and isinstance(a[k], dict) and isinstance(a[k].get("rho"), float) and a[k]["rho"] < 1.0:
                sub = 1e-12 / (1.0 - a[k]["rho"])
            if k in ("p_value", "p_one_sided", "valid_draws", "cbb_valid_draws"):
                sub = 0.0
            out += close(a[k], b[k], f"{path}.{k}", sub)
        return out
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return [f"{path}: length"]
        return [p for i, (x, y) in enumerate(zip(a, b)) for p in close(x, y, f"{path}[{i}]", rel)]
    if isinstance(a, float) and isinstance(b, float):
        if a == b or (math.isnan(a) and math.isnan(b)) or (rel > 0 and math.isclose(a, b, rel_tol=rel, abs_tol=1e-300)):
            return []
        return [f"{path}: {a!r} != {b!r}"]
    return [] if a == b and type(a) is type(b) else [f"{path}: {a!r} != {b!r}"]


def load(path: Path):
    doc = json.loads(path.read_text(encoding="utf-8"))
    for row in doc if isinstance(doc, list) else [doc]:
        row.pop("nav_summ_run", None)
    return doc


def test_pbo_max_splits_has_no_engine_twin(tmp_path, capsys):
    d = cells(tmp_path, 1)[0]
    with pytest.raises(SystemExit):
        NS.main([str(d), "--pbo", "--pbo-max-splits", "10", "--engine-stats", str(tmp_path / "none.exe")])


def test_a_missing_executable_is_refused(tmp_path):
    d = cells(tmp_path, 1)[0]
    with pytest.raises(SystemExit, match="executable"):
        NS.main([str(d), "--engine-stats", str(tmp_path / "none.exe")])


def test_every_statistic_prints_the_same_with_the_engine(tmp_path, capsys):
    need_exe()
    dirs = cells(tmp_path, 6)
    ledger = tmp_path / "fixture-ledger.jsonl"            # never the ledger of record (review B2 S10)
    run([str(d) for d in dirs[:4]] + ["--ledger", str(ledger), "--protocol", "v8", "--origin", "prior"], capsys)
    base = [str(d) for d in dirs[1:]] + ["--reference", str(dirs[0]), "--protocol", "v8", "--draws", "499",
                                         "--psr", "--effective-n", "dirs", "--pbo", "--pbo-blocks", "8",
                                         "--dsr-ledger", str(ledger), "--dsr-total", str(ledger),
                                         "--dsr-hand", str(dirs[2])]
    py_out, py_err = run(base + ["--json", str(tmp_path / "py.json"), "--pbo-json", str(tmp_path / "py_pbo.json")],
                         capsys)
    en_out, en_err = run(base + ["--json", str(tmp_path / "en.json"), "--pbo-json", str(tmp_path / "en_pbo.json"),
                                 "--engine-stats", str(EXE)], capsys)
    assert en_out == py_out
    assert en_err == py_err
    assert close(load(tmp_path / "py.json"), load(tmp_path / "en.json")) == []
    assert close(load(tmp_path / "py_pbo.json"), load(tmp_path / "en_pbo.json")) == []
    assert "DSR_hand" in py_out and "CSCV PBO" in py_out and "effective N" in py_out and "paired vs" in py_out


def test_bundle_and_single_cell_print_the_same_with_the_engine(tmp_path, capsys):
    need_exe()
    dirs = cells(tmp_path, 2, seed=11)
    base = [str(dirs[1]), "--protocol", "v8", "--draws", "299", "--bundle", str(dirs[0]), str(dirs[1])]
    py_out, _ = run(base + ["--bundle-json", str(tmp_path / "py.json")], capsys)
    en_out, _ = run(base + ["--bundle-json", str(tmp_path / "en.json"), "--engine-stats", str(EXE)], capsys)
    assert en_out == py_out
    assert close(load(tmp_path / "py.json"), load(tmp_path / "en.json")) == []
    assert "verdict" in py_out and "Lo (2002)" in py_out


def test_a_pool_row_prints_the_same_with_the_engine(tmp_path, capsys):
    need_exe()
    rng = np.random.default_rng(5)
    e1 = write_nav(tmp_path / "e1", list(0.0004 + 0.008 * rng.standard_normal(80)))
    e2 = write_nav(tmp_path / "e2", list(0.0002 + 0.008 * rng.standard_normal(90)))
    summary = json.loads((e2 / "summary.json").read_text(encoding="utf-8"))
    daily = (e2 / f"daily_{NS.scenario_of(summary)['scenario']}.csv")
    rows = daily.read_text(encoding="utf-8").splitlines()
    shift = 200 * NS.BI.DAY_NS                       # era 2 after era 1
    head, body = rows[0], rows[1:]
    k = head.split(",").index("session_ns")
    moved = []
    for line in body:
        cols = line.split(",")
        cols[k] = str(int(cols[k]) + shift)
        moved.append(",".join(cols))
    daily.write_text("\n".join([head] + moved) + "\n", encoding="utf-8")
    other, ref = cells(tmp_path, 2, t=300, seed=8)
    base = [str(other), "--pool", str(e1), str(e2), "--reference", str(ref), "--draws", "199"]
    py_out, _ = run(base + ["--json", str(tmp_path / "py.json")], capsys)
    en_out, _ = run(base + ["--json", str(tmp_path / "en.json"), "--engine-stats", str(EXE)], capsys)
    assert en_out == py_out
    assert close(load(tmp_path / "py.json"), load(tmp_path / "en.json")) == []


def test_a_dir_paired_with_itself(tmp_path, capsys):
    """Rulings B2-4 and B2-R1 (c): a dir that is its own reference is a degenerate pair (1 - rho = 0 < 1e-6). The
    engine refuses its Memmel SE, t and the LW CI and p (printed "na", null in --json, with a degenerate line); dSR is
    exactly 0. The retired Python printed numpy's corrcoef residue (1 - rho ~ 2e-16, SE ~ 2e-8) instead."""
    need_exe()
    d = cells(tmp_path, 1, seed=13)[0]
    out, _ = run([str(d), "--reference", str(d), "--draws", "99", "--json", str(tmp_path / "s.json"),
                  "--engine-stats", str(EXE)], capsys)
    p = load(tmp_path / "s.json")[0]["paired"]
    assert p["dsr"] == 0.0 and p["sr"] == p["sr_reference"] and 1.0 - p["rho"] <= 4e-16
    assert p["degenerate"] is True and p["one_minus_rho"] == 0.0
    assert p["memmel_se"] is None and p["t"] is None and p["lw"]["se"] is None and p["lw"]["p_value"] is None
    assert p["lw"]["ci95"] == [None, None] and p["lw"]["valid_draws"] == 0
    assert "| Memmel SE na t na |" in out and "] p na (dSR_pop" in out
    assert ("   paired: degenerate pair (1 - rho 0.0e+00 < 1e-06): Memmel SE, t and the LW CI and p are refused "
            "(ruling B2-R1)") in out


EDGES = Path(__file__).resolve().parents[2] / "atx-engine" / "tests" / "fixtures" / "research_eval_edges"


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
    """Ruling B2-R1 on the edge fixture's pairs (1 - rho 5e-4 .. 6e-13): with 1 - rho >= 1e-6 the engine prints the
    Python's lines byte for byte and its --json paired floats are within 1e-12 / (1 - rho), counts equal; below 1e-6
    the pair is refused (SE / t / p "na") where the Python printed numbers."""
    need_exe()
    cell, ref = edge_dirs(tmp_path, case)
    base = [str(cell), "--reference", str(ref), "--draws", "999", "--seed", "20260929"]
    py_out, _ = run(base + ["--json", str(tmp_path / "py.json")], capsys)
    en_out, _ = run(base + ["--json", str(tmp_path / "en.json"), "--engine-stats", str(EXE)], capsys)
    py, en = load(tmp_path / "py.json")[0]["paired"], load(tmp_path / "en.json")[0]["paired"]
    if not case["degenerate"]:
        assert en_out == py_out
        assert close({"paired": py}, {"paired": en}) == []
        assert "degenerate" not in en
        return
    assert en["degenerate"] is True and en["one_minus_rho"] < 1e-6
    assert en["memmel_se"] is None and en["t"] is None and en["lw"]["p_value"] is None
    assert isinstance(py["memmel_se"], float) and py["lw"]["p_value"] is not None
    assert "| Memmel SE na t na |" in en_out and "paired: degenerate pair (1 - rho " in en_out
    assert py_out != en_out


def test_the_engine_run_names_its_verb(tmp_path, capsys):
    """Ruling B2-R3: with the engine every nav_summ_run (--json, --pbo-json, --bundle-json) records the verb's path
    and the SHA-256 of its bytes; without it nav_summ_run is as before (no engine keys)."""
    need_exe()
    dirs = cells(tmp_path, 4, seed=21)
    want = {"engine_exe": str(EXE.resolve()), "engine_exe_sha256": hashlib.sha256(EXE.read_bytes()).hexdigest()}
    run([str(d) for d in dirs] + ["--pbo", "--pbo-blocks", "4", "--json", str(tmp_path / "en.json"), "--pbo-json",
                                  str(tmp_path / "en_pbo.json"), "--engine-stats", str(EXE)], capsys)
    run([str(dirs[1]), "--bundle", str(dirs[0]), str(dirs[1]), "--draws", "99", "--bundle-json",
         str(tmp_path / "en_bundle.json"), "--engine-stats", str(EXE)], capsys)
    for name in ("en.json", "en_pbo.json", "en_bundle.json"):
        doc = json.loads((tmp_path / name).read_text(encoding="utf-8"))
        run_doc = (doc[0] if isinstance(doc, list) else doc)["nav_summ_run"]
        assert {k: run_doc.get(k) for k in want} == want, name
    run([str(dirs[0]), "--json", str(tmp_path / "py.json")], capsys)
    run_doc = json.loads((tmp_path / "py.json").read_text(encoding="utf-8"))[0]["nav_summ_run"]
    assert "engine_exe" not in run_doc and "engine_exe_sha256" not in run_doc
