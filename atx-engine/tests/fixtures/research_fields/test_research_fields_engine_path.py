"""prepare_research_fields_engine.py (platform core migration slice 3; P9 K-P9-1 / K-P9-3): the --engine-fields path.

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py

It lives beside the fixture, not in atx-engine/tools/: the tools' conftest.py binds the superseded research window for
the tools' own fixtures, while this fixture and the engine are on the repository window. Run it in a session that does
not also collect atx-engine/tools (the autouse check below fails loudly otherwise).

On the synthetic identity fixture (this directory), with a synthetic field registry whose ported rows are kind engine:
* flag absent: the wrapper is the plain builder, every output byte identical, and neither the registry nor an
  executable is read;
* flag present with a stand-in executable that replays the Python builder's committed payloads and manifest blocks
  through the receipt contract: every payload byte and coverage block equals the Python path's, each engine entry
  carries the K-P9-3 producer block (FD-1) and that block is the manifests' only difference; each guard (payload
  SHA-256, numpy quantiles, formula fingerprint, source bytes, executable identity, exit status) refuses a disagreeing
  engine; --engine-fields takes only the registry's kind engine rows;
* ATX_RESEARCH_FIELDS_EXE=<build>/bin/atx-research-fields.exe (root, after building the target): the same identity with
  the real executable (skipped when unset).
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

import pytest

FIXTURE = Path(__file__).resolve().parent
HERE = FIXTURE.parents[2] / "tools"          # atx-engine/tools: the builder and its engine wrapper
sys.path.insert(0, str(HERE))
import prepare_research_fields as builder  # noqa: E402
import prepare_research_fields_engine as engine  # noqa: E402
import research_window as rw  # noqa: E402

FIELDS = ["si_shares", "si_dtc", "vol_126"]
GROUPS = {"si_shares": "finra", "si_dtc": "finra", "vol_126": "price_volume"}


@pytest.fixture(autouse=True)
def repository_window():
    """The builder copied the repository window's seal at import (the engine's research_window.hpp is that window)."""
    if builder.SEAL != rw.current()["SEAL"]:
        pytest.fail("the builder was imported under another research window (atx-engine/tools/conftest.py binds the "
                    "superseded one): run this file in its own pytest session")


def registry_row(name: str, kind: str) -> dict:
    """A K-P9-1 row (lane A1's shape): an engine row's builder is the executable's builder-kind id."""
    return {"name": name, "kind": kind, "builder": name if kind == "engine" else "prepare_research_fields",
            "dtype": "f64", "point_in_time": True, "spec_text": builder.spec_definition(name, 1),
            "formula_sha256": builder.formula_id(name, builder.spec_definition(name, 1)), "requires": [],
            "options": {"group": GROUPS.get(name, "role")}, "sources": [], "first_session": None, "owner": "test"}


def write_registry(path: Path, kinds: dict) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    doc = {"schema": "atx.field-registry/v1", "fields": [registry_row(n, k) for n, k in kinds.items()]}
    path.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    return path


@pytest.fixture(autouse=True)
def field_registry(tmp_path, monkeypatch):
    """The wrapper reads this synthetic registry (the repository's field_registry.json is lane A1's)."""
    path = write_registry(tmp_path / "registry" / "field_registry.json",
                          {"mkt_ret": "python", "si_shares": "engine", "si_dtc": "engine", "vol_126": "engine"})
    monkeypatch.setattr(engine, "REGISTRY", path)
    return path


STANDIN = r'''
import hashlib, json, os, sys
from pathlib import Path
sys.path.insert(0, {tools!r})
import prepare_research_fields as b
FIX = Path({fixture!r})
EXE = Path({exe!r})
a = sys.argv[1:]
assert a[0] == "build", a
spec = json.loads(Path(a[a.index("--spec") + 1]).read_text())
mode = os.environ.get("STANDIN_MODE", "")
if mode == "fail":
    sys.exit(1)
m = json.loads((FIX / "expected" / "manifest.normalized.json").read_text())
out, entries = Path(spec["output_dir"]), []
for name in spec["fields"]:
    blob = (FIX / "expected" / (name + ".f64")).read_bytes()
    with (out / (name + ".f64")).open("xb") as f:
        f.write(blob)
    e = next(x for x in m["fields"] if x["name"] == name)
    cov = json.loads(json.dumps(e["coverage"]))
    if mode == "quantile":
        cov["member_finite_quantiles"]["p50"] += 1.0
    formula = b.formula_id(name, b.spec_definition(name, 1))
    sources = [dict(s, path=s["path"].replace("<fixture>", str(FIX))) for s in e["sources"]]
    if mode == "source":
        sources[0]["sha256"] = "0" * 64
    entry = {{"name": name, "file": name + ".f64", "bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest(),
             "formula_sha256": "1" * 64 if mode == "formula" else formula, "sources": sources, "coverage": cov}}
    if name in m["source_checks"]:
        entry["source_checks"] = m["source_checks"][name]
        entry["extra"] = {{"vintage_safe_from": e["vintage_safe_from"]}}
    entries.append(entry)
exe_sha = "0" * 64 if mode == "identity" else hashlib.sha256(EXE.read_bytes()).hexdigest()
receipt = {{"schema": "atx.research-fields-receipt/v1", "status": "complete",
           "engine": {{"name": "stand-in", "fields": spec["fields"], "exe_sha256": exe_sha, "git_sha": "standin-git",
                      "build_type": "StandIn"}},
           "role": {{"manifest_sha256": spec["role"]["manifest_sha256"]}}, "fields": entries}}
with Path(a[a.index("--receipt") + 1]).open("xb") as f:
    f.write(json.dumps(receipt).encode())
'''


def role_sha() -> str:
    return hashlib.sha256((FIXTURE / "role" / "manifest.json").read_bytes()).hexdigest()


def argv(out: Path, fields=FIELDS) -> list:
    return ["--role", str(FIXTURE / "role"), "--role-sha256", role_sha(), "--output", str(out),
            "--fields", ",".join(fields), "--finra", str(FIXTURE / "finra")]


def standin(tmp_path: Path) -> Path:
    exe = tmp_path / "standin.cmd"
    script = tmp_path / "standin.py"
    script.write_text(STANDIN.format(tools=str(HERE), fixture=str(FIXTURE), exe=str(exe)), encoding="utf-8")
    exe.write_text(f'@"{sys.executable}" "{script}" %*\n', encoding="utf-8")
    return exe


def files_of(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*")
            if p.is_file() and p.relative_to(root).parts[0] != engine.RECEIPT_DIR}


def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def producers(out: Path, exe: Path, names, git_sha="standin-git", build_type="StandIn") -> dict:
    """The K-P9-3 block each engine entry must carry: this executable, the receipt of its own call."""
    return {n: {"kind": "engine", "exe_sha256": sha(Path(exe).read_bytes()), "git_sha": git_sha,
                "build_type": build_type,
                "receipt_sha256": sha((out / engine.RECEIPT_DIR / f"{n}.receipt.json").read_bytes())}
            for n in names}


def without_producers(manifest: dict, names) -> dict:
    """The manifest with the named entries' producer blocks removed (the identity claim covers everything else)."""
    m = copy.deepcopy(manifest)
    for e in m["fields"]:
        if e["name"] in names:
            e.pop("producer", None)
    return m


def check_engine_manifest(out: Path, python: Path, exe: Path, names, **identity) -> None:
    """Payloads and coverage equal the Python path's; each engine entry names the engine; nothing else differs."""
    got, want = files_of(out), files_of(python)
    assert sorted(got) == sorted(want)
    for name in FIELDS:
        assert got[f"{name}.f64"] == want[f"{name}.f64"], name
    mg, mw = json.loads(got["manifest.json"]), json.loads(want["manifest.json"])
    entries = {e["name"]: e for e in mg["fields"]}
    assert {n: entries[n]["producer"] for n in names} == producers(out, exe, names, **identity)
    assert {e["name"]: e["coverage"] for e in mg["fields"]} == {e["name"]: e["coverage"] for e in mw["fields"]}
    assert without_producers(mg, names) == without_producers(mw, names)
    python_entries = {e["name"]: e for e in mw["fields"]}
    for name in FIELDS:
        if name not in names:   # a Python-built entry keeps the Python producer shape (no "kind")
            assert "kind" not in python_entries[name].get("producer", {}) and \
                entries[name].get("producer") == python_entries[name].get("producer"), name


def test_flag_absent_is_the_plain_builder(tmp_path, monkeypatch):
    monkeypatch.setattr(engine, "REGISTRY", tmp_path / "absent" / "field_registry.json")   # never read
    engine.main(argv(tmp_path / "wrapped"))
    builder.main(argv(tmp_path / "plain"))
    wrapped, plain = files_of(tmp_path / "wrapped"), files_of(tmp_path / "plain")
    assert wrapped == plain and "manifest.json" in plain
    assert not (tmp_path / "wrapped" / engine.RECEIPT_DIR).exists()
    for name in FIELDS:   # and the plain builder still reproduces the committed fixture payloads
        assert plain[f"{name}.f64"] == (FIXTURE / "expected" / f"{name}.f64").read_bytes()


def test_engine_path_claims_payload_and_coverage_and_names_the_engine(tmp_path):
    exe = standin(tmp_path)
    builder.main(argv(tmp_path / "python"))
    engine.main(argv(tmp_path / "engine") + ["--engine-fields", ",".join(FIELDS), "--engine-exe", str(exe)])
    check_engine_manifest(tmp_path / "engine", tmp_path / "python", exe, FIELDS)
    receipts = sorted(p.name for p in (tmp_path / "engine" / engine.RECEIPT_DIR).iterdir())
    assert receipts == sorted([f"{n}.receipt.json" for n in FIELDS] + [f"{n}.spec.json" for n in FIELDS])
    # The Python path's own entries: vol_126 names its module, the FINRA entries carry no entry producer.
    python = {e["name"]: e for e in json.loads((tmp_path / "python" / "manifest.json").read_text())["fields"]}
    assert python["vol_126"]["producer"]["module"] == "research_fields_price.py"
    assert "producer" not in python["si_shares"] and "producer" not in python["si_dtc"]
    # A subset: the engine computes vol_126 only, the Python the FINRA fields.
    engine.main(argv(tmp_path / "mixed") + ["--engine-fields", "vol_126", "--engine-exe", str(exe)])
    check_engine_manifest(tmp_path / "mixed", tmp_path / "python", exe, ["vol_126"])
    assert sorted(p.name for p in (tmp_path / "mixed" / engine.RECEIPT_DIR).iterdir()) == \
        ["vol_126.receipt.json", "vol_126.spec.json"]


@pytest.mark.parametrize("mode,match", [("quantile", "quantiles differ from numpy"),
                                        ("formula", "formula fingerprint"),
                                        ("source", "other source bytes"),
                                        ("identity", "exe_sha256"),
                                        ("fail", "exit 1")])
def test_guards_refuse_a_disagreeing_engine(tmp_path, monkeypatch, mode, match):
    exe = standin(tmp_path)
    monkeypatch.setenv("STANDIN_MODE", mode)
    with pytest.raises(engine.EngineError, match=match):
        engine.main(argv(tmp_path / "out", ["si_shares"]) + ["--engine-fields", "si_shares", "--engine-exe", str(exe)])
    assert not (tmp_path / "out" / "manifest.json").exists()      # nothing published
    assert builder.finra_field.__name__ == "finra_field" and builder.finra_field.__module__ == builder.__name__


def test_flag_errors(tmp_path, monkeypatch):
    with pytest.raises(engine.EngineError, match="distinct names"):
        engine.main(argv(tmp_path / "a") + ["--engine-fields", "mkt_cap", "--engine-exe", str(tmp_path)])
    with pytest.raises(engine.EngineError, match="distinct names"):   # a python row of the registry
        engine.main(argv(tmp_path / "a") + ["--engine-fields", "mkt_ret", "--engine-exe", str(tmp_path)])
    with pytest.raises(SystemExit):
        engine.main(argv(tmp_path / "b") + ["--engine-fields", "vol_126"])
    with pytest.raises(engine.EngineError, match="not a file"):
        engine.main(argv(tmp_path / "c") + ["--engine-fields", "vol_126", "--engine-exe", str(tmp_path / "none.exe")])
    with pytest.raises(engine.EngineError, match="name fields of --fields"):
        engine.run(FIXTURE / "role", role_sha(), tmp_path / "d", ["si_dtc"], engine_fields=["vol_126"],
                   engine_exe=standin(tmp_path), finra=FIXTURE / "finra", module_options={})
    with pytest.raises(engine.EngineError, match="no engine route"):    # the builder entry's call (A1)
        with engine.engine_path(standin(tmp_path), ["ret_overnight"]):
            pass
    monkeypatch.setattr(engine, "REGISTRY", tmp_path / "absent" / "field_registry.json")
    with pytest.raises(engine.EngineError, match="cannot be read"):
        engine.main(argv(tmp_path / "e") + ["--engine-fields", "vol_126", "--engine-exe", str(standin(tmp_path))])


def test_engine_rows_follow_the_registry(tmp_path, field_registry):
    assert engine.ENGINE_FIELDS == ("si_shares", "si_dtc", "vol_126")   # the routable fields (lane A1 imports it)
    assert engine.engine_rows() == ["si_shares", "si_dtc", "vol_126"]          # registration order, python rows out
    assert engine.engine_rows(field_registry) == ["si_shares", "si_dtc", "vol_126"]
    flipped = write_registry(tmp_path / "other.json", {"vol_126": "engine", "si_dtc": "python", "si_shares": "python"})
    assert engine.engine_rows(flipped) == ["vol_126"]
    with pytest.raises(engine.EngineError, match="distinct names"):
        engine.parse_names("si_dtc", flipped)
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"schema": "atx.field-registry/v0", "fields": []}), encoding="utf-8")
    with pytest.raises(engine.EngineError, match="field registry"):
        engine.engine_rows(bad)


def test_repository_registry_routes_the_ported_rows_to_the_engine():
    """K-P9-1 / ruling P5: in the repository's registry the three ported rows are kind engine, builder = kind id."""
    path = HERE / "field_registry.json"
    if not path.exists():
        pytest.skip("field_registry.json (K-P9-1) lands with lane A1; lane A2's listed edit flips its three rows")
    rows = {r["name"]: r for r in json.loads(path.read_text(encoding="utf-8"))["fields"]}
    for name in FIELDS:
        assert rows[name]["kind"] == "engine" and rows[name]["builder"] == name, rows[name]
        assert rows[name]["dtype"] == "f64"
        assert rows[name]["formula_sha256"] == builder.formula_id(name, builder.spec_definition(name, 1))
    assert set(engine.engine_rows(path)) >= set(FIELDS)


@pytest.mark.skipif(not os.environ.get("ATX_RESEARCH_FIELDS_EXE"), reason="set ATX_RESEARCH_FIELDS_EXE (root)")
def test_real_executable_identity(tmp_path):
    exe = Path(os.environ["ATX_RESEARCH_FIELDS_EXE"])
    builder.main(argv(tmp_path / "python"))
    engine.main(argv(tmp_path / "engine") + ["--engine-fields", ",".join(FIELDS), "--engine-exe", str(exe)])
    receipt = json.loads((tmp_path / "engine" / engine.RECEIPT_DIR / "vol_126.receipt.json").read_text())
    assert receipt["engine"]["name"] == "atx-research-fields"
    check_engine_manifest(tmp_path / "engine", tmp_path / "python", exe, FIELDS,
                          git_sha=receipt["engine"]["git_sha"], build_type=receipt["engine"]["build_type"])
    shutil.rmtree(tmp_path / "engine")
