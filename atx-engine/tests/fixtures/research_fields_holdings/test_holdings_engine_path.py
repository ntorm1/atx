"""The engine route of the holdings fields (P9 lane A4, migration slice 5) on the holdings identity fixture.

``prepare_research_fields_engine.run`` with ``engine_fields`` ftd_shares_ratio21 / regsho_threshold_days63 and the real
executable (``ATX_RESEARCH_FIELDS_EXE=<build>/bin/atx-research-fields.exe``; skipped when unset) against the plain Python
run of the same recipe on the same committed stages: every output byte is the Python run's, and the manifests differ only
by each routed entry's producer block (the Python path's names research_fields_holdings.py, the engine path's is the
K-P9-3 block naming the executable). Flag absent, the wrapper is the plain builder.

  ATX_RESEARCH_FIELDS_EXE=... "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/research_fields_holdings/test_holdings_engine_path.py

Run it in its own pytest session (the tools' conftest binds another research window).
"""
from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_holdings_fixture as fx  # noqa: E402

base_fx, builder, hold, rw, base, t2 = fx._modules()
import prepare_research_fields_engine as engine  # noqa: E402  (atx-engine/tools is on the path now)

PORTED = ["ftd_shares_ratio21", "regsho_threshold_days63"]
REAL = pytest.mark.skipif(not os.environ.get("ATX_RESEARCH_FIELDS_EXE"), reason="set ATX_RESEARCH_FIELDS_EXE")


def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


@pytest.fixture(autouse=True)
def repository_window():
    if builder.SEAL != rw.current()["SEAL"]:
        pytest.fail("the builder was imported under another research window: run this file in its own pytest session")


def use_registry(tmp_path: Path, monkeypatch) -> None:
    """A registry for the wrapper's --engine-fields check whose PORTED rows are kind engine."""
    rows = [{"name": x, "kind": "engine" if x in PORTED else "python"} for x in t2.RECIPE]
    path = tmp_path / "registry" / "field_registry.json"
    path.parent.mkdir(parents=True)
    path.write_text(json.dumps({"schema": engine.REGISTRY_SCHEMA, "fields": rows}), encoding="utf-8")
    monkeypatch.setattr(engine, "REGISTRY", path)


def run(tmp_path: Path, out: Path, engine_fields=None, exe=None) -> dict:
    """The fixture's recipe on the committed stages and role; the builder-only inputs (written once by their writers
    under tmp_path/inputs) are shared by every run of a test, so the manifests name the same input paths."""
    work = tmp_path / "inputs"
    th = work / "TickerHistory3.parquet"
    if not work.exists():
        work.mkdir(parents=True)
        base.write_tickerhistory(th)
        base.write_finra(work / "finra")
        base.write_lake(work / "lake")
    stages = {}
    for key in fx.STAGES:
        stages[key] = HERE / key
        stages[key + "_sha256"] = sha((HERE / key / "manifest.json").read_bytes())
    role_sha = sha((HERE / "role" / "manifest.json").read_bytes())
    with contextlib.redirect_stdout(io.StringIO()):
        return engine.run(HERE / "role", role_sha, out, list(t2.RECIPE), engine_fields=engine_fields,
                          engine_exe=exe, finra=work / "finra", tickerhistory=th, lake=work / "lake", **stages)


def rooted(value, root: str):
    """``value`` with every path under the run's output directory ``root`` written as <output>/..., nothing dropped."""
    if isinstance(value, dict):
        return {k: rooted(v, root) for k, v in value.items()}
    if isinstance(value, list):
        return [rooted(v, root) for v in value]
    if isinstance(value, str) and value.lower().startswith(root.lower()):
        return "<output>/" + Path(value[len(root):].lstrip("\\/")).as_posix()
    return value


def files_of(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*")
            if p.is_file() and p.relative_to(root).parts[0] != engine.RECEIPT_DIR}


def test_flag_absent_is_the_plain_builder(tmp_path):
    run(tmp_path, tmp_path / "plain")
    got = files_of(tmp_path / "plain")
    for name in PORTED:
        assert got[f"{name}.f64"] == (HERE / "expected" / f"{name}.f64").read_bytes(), name
    assert not (tmp_path / "plain" / engine.RECEIPT_DIR).exists()


@REAL
def test_real_executable_identity(tmp_path, monkeypatch):
    use_registry(tmp_path, monkeypatch)
    exe = Path(os.environ["ATX_RESEARCH_FIELDS_EXE"])
    run(tmp_path, tmp_path / "python")
    run(tmp_path, tmp_path / "engine", engine_fields=PORTED, exe=exe)
    got, want = files_of(tmp_path / "engine"), files_of(tmp_path / "python")
    assert sorted(got) == sorted(want)
    for rel in want:
        if rel != "manifest.json":
            assert got[rel] == want[rel], rel
    for name in PORTED:
        assert got[f"{name}.f64"] == (HERE / "expected" / f"{name}.f64").read_bytes(), name
    # each run's own output directory (this run's shares_out.f64 is a source) is the one path that may differ
    mg = rooted(json.loads(got["manifest.json"]), str((tmp_path / "engine").resolve()))
    mw = rooted(json.loads(want["manifest.json"]), str((tmp_path / "python").resolve()))
    entries = {e["name"]: e for e in mg["fields"]}
    receipts = sorted((tmp_path / "engine" / engine.RECEIPT_DIR).glob("*.receipt.json"))
    assert [p.name for p in receipts] == [f"{name}.receipt.json" for name in PORTED]
    for name, receipt in zip(PORTED, receipts):
        doc = json.loads(receipt.read_text(encoding="utf-8"))
        assert doc["engine"]["name"] == "atx-research-fields"
        assert entries[name]["producer"] == {"kind": "engine", "exe_sha256": sha(exe.read_bytes()),
                                             "git_sha": doc["engine"]["git_sha"],
                                             "build_type": doc["engine"]["build_type"],
                                             "receipt_sha256": sha(receipt.read_bytes())}, name
    python_entries = {e["name"]: e for e in mw["fields"]}
    for name in PORTED:   # the Python path names its module there; the engine path names the executable
        assert python_entries[name]["producer"]["module"] == "research_fields_holdings.py", name
    stripped = []
    for m in (mg, mw):
        m = copy.deepcopy(m)
        for e in m["fields"]:
            if e["name"] in PORTED:
                e.pop("producer", None)
        stripped.append(m)
    assert stripped[0] == stripped[1]
