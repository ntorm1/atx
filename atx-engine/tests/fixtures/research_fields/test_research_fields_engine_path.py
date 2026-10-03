"""prepare_research_fields_engine.py (platform core migration slice 3): the --engine-fields path.

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py

It lives beside the fixture, not in atx-engine/tools/: the tools' conftest.py binds the superseded research window for
the tools' own fixtures, while this fixture and the engine are on the repository window. Run it in a session that does
not also collect atx-engine/tools (the autouse check below fails loudly otherwise).

On the synthetic identity fixture (this directory):
* flag absent: the wrapper is the plain builder, every output byte identical;
* flag present with a stand-in executable that replays the Python builder's committed payloads and manifest blocks
  through the receipt contract: the manifest is byte-identical to the Python path's, and each guard (payload SHA-256,
  numpy quantiles, formula fingerprint, source bytes, exit status) refuses a disagreeing engine;
* ATX_RESEARCH_FIELDS_EXE=<build>/bin/atx-research-fields.exe (root, after building the target): the same identity with
  the real executable (skipped when unset).
"""
from __future__ import annotations

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


@pytest.fixture(autouse=True)
def repository_window():
    """The builder copied the repository window's seal at import (the engine's research_window.hpp is that window)."""
    if builder.SEAL != rw.current()["SEAL"]:
        pytest.fail("the builder was imported under another research window (atx-engine/tools/conftest.py binds the "
                    "superseded one): run this file in its own pytest session")

STANDIN = r'''
import hashlib, json, os, sys
from pathlib import Path
sys.path.insert(0, {tools!r})
import prepare_research_fields as b
FIX = Path({fixture!r})
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
receipt = {{"schema": "atx.research-fields-receipt/v1", "status": "complete", "engine": {{"name": "stand-in"}},
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
    script = tmp_path / "standin.py"
    script.write_text(STANDIN.format(tools=str(HERE), fixture=str(FIXTURE)), encoding="utf-8")
    exe = tmp_path / "standin.cmd"
    exe.write_text(f'@"{sys.executable}" "{script}" %*\n', encoding="utf-8")
    return exe


def files_of(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*")
            if p.is_file() and p.relative_to(root).parts[0] != engine.RECEIPT_DIR}


def test_flag_absent_is_the_plain_builder(tmp_path):
    engine.main(argv(tmp_path / "wrapped"))
    builder.main(argv(tmp_path / "plain"))
    wrapped, plain = files_of(tmp_path / "wrapped"), files_of(tmp_path / "plain")
    assert wrapped == plain and "manifest.json" in plain
    assert not (tmp_path / "wrapped" / engine.RECEIPT_DIR).exists()
    for name in FIELDS:   # and the plain builder still reproduces the committed fixture payloads
        assert plain[f"{name}.f64"] == (FIXTURE / "expected" / f"{name}.f64").read_bytes()


def test_engine_path_manifest_is_byte_identical(tmp_path):
    exe = standin(tmp_path)
    builder.main(argv(tmp_path / "python"))
    engine.main(argv(tmp_path / "engine") + ["--engine-fields", ",".join(FIELDS), "--engine-exe", str(exe)])
    assert files_of(tmp_path / "engine") == files_of(tmp_path / "python")
    receipts = sorted(p.name for p in (tmp_path / "engine" / engine.RECEIPT_DIR).iterdir())
    assert receipts == sorted([f"{n}.receipt.json" for n in FIELDS] + [f"{n}.spec.json" for n in FIELDS])
    # A subset: the engine computes vol_126 only, the Python the FINRA fields; same bytes again.
    engine.main(argv(tmp_path / "mixed") + ["--engine-fields", "vol_126", "--engine-exe", str(exe)])
    assert files_of(tmp_path / "mixed") == files_of(tmp_path / "python")
    assert sorted(p.name for p in (tmp_path / "mixed" / engine.RECEIPT_DIR).iterdir()) == \
        ["vol_126.receipt.json", "vol_126.spec.json"]


@pytest.mark.parametrize("mode,match", [("quantile", "quantiles differ from numpy"),
                                        ("formula", "formula fingerprint"),
                                        ("source", "other source bytes"),
                                        ("fail", "exit 1")])
def test_guards_refuse_a_disagreeing_engine(tmp_path, monkeypatch, mode, match):
    exe = standin(tmp_path)
    monkeypatch.setenv("STANDIN_MODE", mode)
    with pytest.raises(engine.EngineError, match=match):
        engine.main(argv(tmp_path / "out", ["si_shares"]) + ["--engine-fields", "si_shares", "--engine-exe", str(exe)])
    assert not (tmp_path / "out" / "manifest.json").exists()      # nothing published
    assert builder.finra_field.__name__ == "finra_field" and builder.finra_field.__module__ == builder.__name__


def test_flag_errors(tmp_path):
    with pytest.raises(engine.EngineError, match="distinct names"):
        engine.main(argv(tmp_path / "a") + ["--engine-fields", "mkt_cap", "--engine-exe", str(tmp_path)])
    with pytest.raises(SystemExit):
        engine.main(argv(tmp_path / "b") + ["--engine-fields", "vol_126"])
    with pytest.raises(engine.EngineError, match="not a file"):
        engine.main(argv(tmp_path / "c") + ["--engine-fields", "vol_126", "--engine-exe", str(tmp_path / "none.exe")])
    with pytest.raises(engine.EngineError, match="name fields of --fields"):
        engine.run(FIXTURE / "role", role_sha(), tmp_path / "d", ["si_dtc"], engine_fields=["vol_126"],
                   engine_exe=standin(tmp_path), finra=FIXTURE / "finra", module_options={})


@pytest.mark.skipif(not os.environ.get("ATX_RESEARCH_FIELDS_EXE"), reason="set ATX_RESEARCH_FIELDS_EXE (root)")
def test_real_executable_identity(tmp_path):
    exe = Path(os.environ["ATX_RESEARCH_FIELDS_EXE"])
    builder.main(argv(tmp_path / "python"))
    engine.main(argv(tmp_path / "engine") + ["--engine-fields", ",".join(FIELDS), "--engine-exe", str(exe)])
    assert files_of(tmp_path / "engine") == files_of(tmp_path / "python")
    receipt = json.loads((tmp_path / "engine" / engine.RECEIPT_DIR / "vol_126.receipt.json").read_text())
    assert receipt["engine"]["name"] == "atx-research-fields"
    shutil.rmtree(tmp_path / "engine")
