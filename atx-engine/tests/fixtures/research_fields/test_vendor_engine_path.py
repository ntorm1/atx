"""prepare_research_fields_engine.py: the --engine-fields path of the six vendor-panel fields (P9 lane A3, ruling A3-1).

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/research_fields/test_vendor_engine_path.py

Beside the fixture for the reason test_research_fields_engine_path.py gives (the repository research window). On the
vendor fixture (vendor/: the parquet, the role projected from it, the Python builder's six payloads and manifest), with
the ohlc draft module bound as its shim binds it and a synthetic field registry whose six rows are kind engine:
* a stand-in executable replays the committed Python payloads and entry blocks through the receipt contract, reporting
  the panel's read statistics under the engine's names: every payload byte and coverage block equals the Python path's,
  each routed entry carries the K-P9-3 producer block of its own call and that block is the manifests' only difference;
  the engine is called once per panel (the routed price fields together, the routed bars together);
* the panel guard refuses an engine whose read statistics differ from the module's panel;
* ATX_RESEARCH_FIELDS_EXE=<build>/bin/atx-research-fields.exe (root, after building the target): the same identity with
  the real executable, for all six and for a subset; and a run whose Python panel reaches further back than the
  engine's own fields ask (ret_overnight routed, ceq_iss_5y left to Python) is refused, never silently different.
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import sys
from unittest import mock

import pytest

FIXTURE = Path(__file__).resolve().parent
VENDOR = FIXTURE / "vendor"
HERE = FIXTURE.parents[2] / "tools"          # atx-engine/tools
sys.path.insert(0, str(HERE))
import field_registry as fr  # noqa: E402
import prepare_research_fields as builder  # noqa: E402
import prepare_research_fields_engine as engine  # noqa: E402
import research_fields_ohlc as ohlc  # noqa: E402
import research_window as rw  # noqa: E402

FIELDS = ["ret_overnight", "ret_intraday", "ceq_iss_5y", "open_adj", "high_adj", "low_adj"]
PRICE = ["ret_overnight", "ret_intraday", "ceq_iss_5y"]
BARS = ["open_adj", "high_adj", "low_adj"]


@pytest.fixture(autouse=True)
def repository_window():
    if builder.SEAL != rw.current()["SEAL"]:
        pytest.fail("the builder was imported under another research window (atx-engine/tools/conftest.py binds the "
                    "superseded one): run this file in its own pytest session")


@pytest.fixture(autouse=True)
def ohlc_bound():
    """The ohlc draft module bound into the builder as its shim binds it (undone after the test)."""
    with mock.patch.dict(builder.ALL_FIELDS), mock.patch.object(builder, "FIELD_MODULES", list(builder.FIELD_MODULES)):
        fr.bind_modules(vars(builder), (ohlc,))
        yield


def use_registry(tmp_path: Path, monkeypatch, engine_rows) -> None:
    """A synthetic registry for the wrapper's --engine-fields check: ``engine_rows`` kind engine, the rest python."""
    rows = [{"name": x, "kind": "engine" if x in engine_rows else "python"} for x in FIELDS]
    path = tmp_path / "registry" / "field_registry.json"
    path.parent.mkdir(parents=True)
    path.write_text(json.dumps({"schema": engine.REGISTRY_SCHEMA, "fields": rows}), encoding="utf-8")
    monkeypatch.setattr(engine, "REGISTRY", path)


STANDIN = r'''
import hashlib, json, os, sys
from pathlib import Path
VENDOR = Path({vendor!r})
EXE = Path({exe!r})
PRICE = {price!r}
SCAN = {{"rows_scanned": "rows_in_file", "rows_on_or_after_seal_skipped": "rows_sealed_dropped"}}
COMMON = {{"caveats", "clock", "coverage", "definition", "dtype", "file", "formula_id", "formula_sha256", "layout",
          "min_history", "name", "non_pit_aspects", "point_in_time", "sha256", "shape", "source_columns", "sources",
          "staleness", "units"}}
a = sys.argv[1:]
spec = json.loads(Path(a[a.index("--spec") + 1]).read_text())
mode = os.environ.get("STANDIN_MODE", "")
m = json.loads((VENDOR / "expected" / "manifest.normalized.json").read_text())
assert Path(spec["price_source"]).read_bytes() == (VENDOR / "th.parquet").read_bytes()
out, entries = Path(spec["output_dir"]), []
for name in spec["fields"]:
    blob = (VENDOR / "expected" / (name + ".f64")).read_bytes()
    with (out / (name + ".f64")).open("xb") as f:
        f.write(blob)
    e = next(x for x in m["fields"] if x["name"] == name)
    group = m["source_checks"]["price" if name in PRICE else "ohlc"]
    scan = {{SCAN.get(k, k): v for k, v in group["source"].items() if k != "rule"}}
    if mode == "panel":
        scan["rows_selected"] += 1
    checks = {{"lag_sessions": group["lag_sessions"], "clock": group["clock"], "source": scan}}
    entries.append({{"name": name, "file": name + ".f64", "bytes": len(blob),
                    "sha256": hashlib.sha256(blob).hexdigest(), "formula_sha256": e["formula_sha256"],
                    "coverage": e["coverage"],
                    "sources": [dict(s, path=s["path"].replace("<fixture>", str(VENDOR))) for s in e["sources"]],
                    "extra": {{k: v for k, v in e.items() if k not in COMMON}}, "source_checks": checks}})
exe_sha = hashlib.sha256(EXE.read_bytes()).hexdigest()
receipt = {{"schema": "atx.research-fields-receipt/v1", "status": "complete",
           "engine": {{"name": "stand-in", "fields": spec["fields"], "exe_sha256": exe_sha,
                      "git_sha": "standin-git", "build_type": "StandIn"}},
           "role": {{"manifest_sha256": spec["role"]["manifest_sha256"]}}, "fields": entries}}
with Path(a[a.index("--receipt") + 1]).open("xb") as f:
    f.write(json.dumps(receipt).encode())
'''


def standin(tmp_path: Path) -> Path:
    exe = tmp_path / "standin.cmd"
    script = tmp_path / "standin.py"
    script.write_text(STANDIN.format(vendor=str(VENDOR), exe=str(exe), price=PRICE), encoding="utf-8")
    exe.write_text(f'@"{sys.executable}" "{script}" %*\n', encoding="utf-8")
    return exe


def role_sha() -> str:
    return hashlib.sha256((VENDOR / "role" / "manifest.json").read_bytes()).hexdigest()


def run(out: Path, engine_fields=None, exe=None, fields=FIELDS):
    return engine.run(VENDOR / "role", role_sha(), out, list(fields), engine_fields=engine_fields, engine_exe=exe,
                      price_source=VENDOR / "th.parquet")


def files_of(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*")
            if p.is_file() and p.relative_to(root).parts[0] != engine.RECEIPT_DIR}


def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def calls(names) -> list:
    """The engine calls the wrapper makes for routed ``names`` of a run of every field: the routed price fields in one,
    the routed bars in another (receipt tags, run order)."""
    return [t for t in ("+".join(x for x in PRICE if x in names), "+".join(x for x in BARS if x in names)) if t]


def check_engine_manifest(out: Path, python: Path, exe: Path, names, git_sha="standin-git", build_type="StandIn"):
    got, want = files_of(out), files_of(python)
    assert sorted(got) == sorted(want)
    for name in FIELDS:
        assert got[f"{name}.f64"] == want[f"{name}.f64"] == (VENDOR / "expected" / f"{name}.f64").read_bytes(), name
    mg, mw = json.loads(got["manifest.json"]), json.loads(want["manifest.json"])
    entries = {e["name"]: e for e in mg["fields"]}
    receipts = {t: sha((out / engine.RECEIPT_DIR / f"{t}.receipt.json").read_bytes()) for t in calls(names)}
    for name in names:
        tag = next(t for t in receipts if name in t.split("+"))
        assert entries[name]["producer"] == {"kind": "engine", "exe_sha256": sha(Path(exe).read_bytes()),
                                             "git_sha": git_sha, "build_type": build_type,
                                             "receipt_sha256": receipts[tag]}, name
    stripped = []
    for m in (mg, mw):
        m = copy.deepcopy(m)
        for e in m["fields"]:
            if e["name"] in names:
                e.pop("producer", None)
        stripped.append(m)
    assert stripped[0] == stripped[1]
    tags = sorted(p.name for p in (out / engine.RECEIPT_DIR).iterdir())
    assert tags == sorted([f"{t}.receipt.json" for t in calls(names)] + [f"{t}.spec.json" for t in calls(names)])


@pytest.mark.parametrize("names", [FIELDS, ["ceq_iss_5y", "high_adj"]])
def test_vendor_fields_route_one_call_per_panel(tmp_path, monkeypatch, names):
    use_registry(tmp_path, monkeypatch, FIELDS)
    exe = standin(tmp_path)
    run(tmp_path / "python")
    run(tmp_path / "engine", engine_fields=names, exe=exe)
    check_engine_manifest(tmp_path / "engine", tmp_path / "python", exe, names)
    assert engine.price.open_return_rows.__module__ == "research_fields_price"   # every hook undone
    assert engine.ohlc.bar_rows.__module__ == "research_fields_ohlc" and engine.ohlc.bar_panel.__name__ == "bar_panel"


@pytest.mark.parametrize("names,match", [(["ret_overnight"], "ret_overnight: the engine's vendor panel differs"),
                                         (["low_adj"], "low_adj: the engine's vendor panel differs")])
def test_panel_guard_refuses_another_panel(tmp_path, monkeypatch, names, match):
    use_registry(tmp_path, monkeypatch, FIELDS)
    monkeypatch.setenv("STANDIN_MODE", "panel")
    with pytest.raises(engine.EngineError, match=match):
        run(tmp_path / "out", engine_fields=names, exe=standin(tmp_path))
    assert not (tmp_path / "out" / "manifest.json").exists()


def test_repository_registry_routes_the_vendor_rows_to_the_engine():
    """K-P9-1 / ruling P5, P9 A3's flip: the six vendor-panel rows are kind engine, builder = their kind id, and keep
    their python twins' formula fingerprint (each module fingerprints its spec at its own lag)."""
    rows = {r["name"]: r for r in json.loads((HERE / "field_registry.json").read_text(encoding="utf-8"))["fields"]}
    for name in FIELDS:
        assert rows[name]["kind"] == "engine" and rows[name]["builder"] == name, rows[name]
        assert rows[name]["dtype"] == "f64" and "engine-shim" in rows[name]["owner"]
        assert rows[name]["formula_sha256"] == builder.formula_id(
            name, builder.spec_definition(name, engine.lag_of(name)))
    assert set(engine.engine_rows(HERE / "field_registry.json")) >= set(FIELDS)


REAL = pytest.mark.skipif(not os.environ.get("ATX_RESEARCH_FIELDS_EXE"), reason="set ATX_RESEARCH_FIELDS_EXE (root)")


@REAL
@pytest.mark.parametrize("names", [FIELDS, ["ceq_iss_5y", "open_adj"]])
def test_real_executable_identity(tmp_path, monkeypatch, names):
    use_registry(tmp_path, monkeypatch, FIELDS)
    exe = Path(os.environ["ATX_RESEARCH_FIELDS_EXE"])
    run(tmp_path / "python")
    run(tmp_path / "engine", engine_fields=names, exe=exe)
    receipt = json.loads((tmp_path / "engine" / engine.RECEIPT_DIR / f"{calls(names)[0]}.receipt.json").read_text())
    assert receipt["engine"]["name"] == "atx-research-fields"
    check_engine_manifest(tmp_path / "engine", tmp_path / "python", exe, names,
                          git_sha=receipt["engine"]["git_sha"], build_type=receipt["engine"]["build_type"])


@REAL
def test_real_executable_refuses_a_longer_python_panel(tmp_path, monkeypatch):
    """ret_overnight routed, ceq_iss_5y left to Python: the module's panel carries ceq_iss_5y's 1,261-session history,
    the engine's call (ret_overnight alone) two sessions; their payload bits may differ, so the guard refuses."""
    use_registry(tmp_path, monkeypatch, ["ret_overnight"])
    with pytest.raises(engine.EngineError, match="ret_overnight: the engine's vendor panel differs"):
        run(tmp_path / "out", engine_fields=["ret_overnight"], exe=Path(os.environ["ATX_RESEARCH_FIELDS_EXE"]),
            fields=["ret_overnight", "ceq_iss_5y"])


def test_panel_guard_fails_closed_on_missing_statistics():
    """Review S-3: ``same_panel`` compares nothing when either side has no statistics, so it refuses: an empty, partial
    or non-dict Python panel record, and an engine entry without its panel block. Complete equal statistics pass."""
    m = json.loads((VENDOR / "expected" / "manifest.normalized.json").read_text(encoding="utf-8"))
    stats = m["source_checks"]["price"]["source"]
    entry = {"source_checks": {"source": {engine.SCAN_KEYS.get(k, k): v for k, v in stats.items() if k != "rule"}}}
    engine.same_panel("ret_overnight", entry, stats)   # complete and equal: accepted
    partial = {k: v for k, v in stats.items() if k != "rows_selected"}
    for bad in ({}, None, partial, {"rule": stats["rule"]}):
        with pytest.raises(engine.EngineError, match="ret_overnight: this builder's vendor panel recorded no read"):
            engine.same_panel("ret_overnight", entry, bad)
    for empty in ({}, {"source_checks": {}}, {"source_checks": {"source": {}}}, {"source_checks": {"source": None}}):
        with pytest.raises(engine.EngineError, match="ret_overnight: the engine's receipt carries no vendor panel"):
            engine.same_panel("ret_overnight", empty, stats)
    bars = m["source_checks"]["ohlc"]["source"]          # the ohlc panel records every PANEL_KEYS key too
    engine.same_panel("open_adj", {"source_checks": {"source": {engine.SCAN_KEYS.get(k, k): v for k, v in
                                                                 bars.items() if k != "rule"}}}, bars)
