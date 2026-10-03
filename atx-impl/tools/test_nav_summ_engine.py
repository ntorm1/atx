"""nav_summ.py --engine-stats (platform P9 B2, K-P9-5): the engine verb atx-research-eval in place of the Python
statistics of record prints the same lines and writes the same JSON keys, values within 1e-12 (the P12 tolerance).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_nav_summ_engine.py

Synthetic NAV dirs only (test_nav_summ.write_nav). The verb is the built atx-research-eval.exe: ATX_EQUITY_BIN, else
build-equity/bin of this checkout; without it the comparisons skip (the argument checks still run). With
ATX_ENGINE_STATS_REQUIRED=1 a missing executable fails instead.
"""
from __future__ import annotations

import json
import math
import os
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


def close(a, b, path="") -> list[str]:
    """Differences of two JSON values: equal structure, ints and strings; floats within 1e-12 (NaN == NaN)."""
    if isinstance(a, dict) and isinstance(b, dict):
        if set(a) != set(b):
            return [f"{path}: keys {sorted(set(a) ^ set(b))}"]
        return [p for k in a for p in close(a[k], b[k], f"{path}.{k}")]
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return [f"{path}: length"]
        return [p for i, (x, y) in enumerate(zip(a, b)) for p in close(x, y, f"{path}[{i}]")]
    if isinstance(a, float) and isinstance(b, float):
        if a == b or (math.isnan(a) and math.isnan(b)) or math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-300):
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
    with pytest.raises(SystemExit, match="no executable"):
        NS.main([str(d), "--engine-stats", str(tmp_path / "none.exe")])


def test_every_statistic_prints_the_same_with_the_engine(tmp_path, capsys):
    need_exe()
    dirs = cells(tmp_path, 6)
    ledger = tmp_path / "trials.jsonl"
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
