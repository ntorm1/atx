"""A0 check harness: switches S1/S2, the pin-safe S3/S4 stage runner, report pieces, frozen code.

Synthetic and offline: tiny parquet fixtures under ``tmp_path``; no lake data, no network.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
import types
from pathlib import Path

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

import atx_db
from atx_db import identity_links as il
from atx_db import identity_reconstruction as ir
from atx_db.alpha_panel import a0_check as a0
from atx_db.alpha_panel import common as C

SRC = Path(atx_db.__file__).resolve().parents[1]
TICKERS = (("1", "AAA"),)


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _copy(query: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect()
    try:
        con.execute(f"COPY ({query}) TO '{dest.as_posix()}' (FORMAT PARQUET)")
    finally:
        con.close()


# ---------------------------------------------------------------------------------------------
# S1: reconstruct passes tickers=() and the switch is bound into the preparation pin


@pytest.fixture
def retained(tmp_path, monkeypatch):
    """A receipt over four tiny input files, with the heavy reconstruction kernels stubbed out."""
    inputs = {}
    for name in ("ticker_history", "companyfacts", "submissions", "company_tickers"):
        path = tmp_path / "inputs" / f"{name}.bin"
        if name == "ticker_history":
            _copy("SELECT 7::BIGINT AS securityID, '0' AS earnFlag", path)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        inputs[name] = {"path": str(path), "sha256": _sha(path), "received_at": "2026-09-20T00:00:00",
                        "receipt_basis": "file_mtime"}
    receipt = tmp_path / "receipt.json"
    receipt.write_text(json.dumps({"inputs": inputs}), encoding="utf-8")
    calls = []

    def stage_vendor(con, path, chunks):
        con.execute("CREATE TABLE ri_vendor_lines AS SELECT 7::BIGINT AS vendor_id")

    def reconstruct(con, lines, **kwargs):
        calls.append(kwargs["tickers"])
        return iter(())

    monkeypatch.setattr(ir, "stage_vendor_ticker_history", stage_vendor)
    monkeypatch.setattr(ir, "iter_companyfacts_share_facts", lambda path: iter(()))
    monkeypatch.setattr(ir, "stage_share_facts", lambda con, facts, batch: 0)
    monkeypatch.setattr(ir, "read_vendor_lines", lambda con: [])
    monkeypatch.setattr(ir, "read_lifecycle_filings", lambda path, ciks: [])
    monkeypatch.setattr(ir, "read_sec_ticker_snapshot", lambda path: TICKERS)
    monkeypatch.setattr(ir, "reconstruct_in_batches", reconstruct)
    return receipt, calls


def _prepare(work: Path, receipt: Path) -> dict:
    for step in ("vendor", "facts", "reconstruct"):
        il.prepare_rehearsal_inputs(work, receipt, step)
    return json.loads((work / "preparation.json").read_text(encoding="utf-8"))


def test_s1_off_passes_snapshot_tickers_and_leaves_the_pin_as_today(retained, tmp_path, monkeypatch):
    receipt, calls = retained
    monkeypatch.delenv("ATX_A0_S1", raising=False)
    manifest = _prepare(tmp_path / "W_off", receipt)
    assert calls == [TICKERS]
    assert set(manifest["pin"]) == {"version", "reconstruction_method", "reconstruction_code_sha256",
                                    "params_digest", "inputs"}
    monkeypatch.setenv("ATX_A0_S1", "0")
    _prepare(tmp_path / "W_zero", receipt)
    assert calls == [TICKERS, TICKERS]
    assert (json.loads((tmp_path / "W_zero" / "preparation.json").read_text(encoding="utf-8"))["pin"]
            == manifest["pin"])


def test_s1_on_passes_empty_tickers_and_binds_the_work_dir(retained, tmp_path, monkeypatch):
    receipt, calls = retained
    monkeypatch.setenv("ATX_A0_S1", "1")
    manifest = _prepare(tmp_path / "W_on", receipt)
    assert calls == [()]
    assert manifest["pin"]["a0_s1"] is True
    monkeypatch.delenv("ATX_A0_S1")
    with pytest.raises(ValueError, match="pins changed"):
        il.prepare_rehearsal_inputs(tmp_path / "W_on", receipt, "reconstruct")
    _prepare(tmp_path / "W_off", receipt)
    monkeypatch.setenv("ATX_A0_S1", "1")
    with pytest.raises(ValueError, match="pins changed"):
        il.prepare_rehearsal_inputs(tmp_path / "W_off", receipt, "reconstruct")
    monkeypatch.setenv("ATX_A0_S1", "yes")
    with pytest.raises(ValueError, match="ATX_A0_S1"):
        il.prepare_rehearsal_inputs(tmp_path / "W_bad", receipt, "vendor")


# ---------------------------------------------------------------------------------------------
# S2: identity_backfill builds an empty candidate set


@pytest.fixture
def backfill_lake(tmp_path, monkeypatch):
    root, r4 = tmp_path / "P", tmp_path / "R"
    _copy("SELECT CAST(d AS DATE) AS session_date FROM range(DATE '2026-08-03', DATE '2026-09-19', INTERVAL 1 DAY) t(d) "
          "WHERE isodow(d) <= 5", root / "calendar.parquet")
    _copy("SELECT 101::BIGINT AS security_id, CAST(d AS DATE) AS session_date FROM range(DATE '2026-08-03', "
          "DATE '2026-09-19', INTERVAL 1 DAY) t(d) WHERE isodow(d) <= 5", root / "prices" / "year=2026" / "prices.parquet")
    _copy("""SELECT 202::BIGINT AS security_id, 2::BIGINT AS cik, DATE '2026-08-03' AS "start", DATE '2026-08-10' AS end_incl,
                    'P' AS "primary", 'high' AS tier, 'strict_dated' AS basis, TIMESTAMP '2026-08-01 00:00' AS available_at""",
          root / "identity" / "links.parquet")
    _copy("""SELECT 101::BIGINT AS perm_security_id, '1' AS cik, 'current_ticker_verified' AS link_basis, 'P' AS link_primary,
                    CAST(NULL AS TIMESTAMP) AS valid_until, TIMESTAMP '2026-09-20 22:00' AS available_at""",
          r4 / "phases" / "export-001" / "security_company_links.part-0000.parquet")
    _copy("SELECT 1::BIGINT AS cik, '10-K' AS form, DATE '2020-01-15' AS filed", tmp_path / "fsds" / "sub" / "a.parquet")
    _copy("SELECT 3::BIGINT AS cik, DATE '2021-01-15' AS filed_date, '10-Q' AS form, 'us-gaap' AS taxonomy",
          tmp_path / "cf" / "batch-0000.parquet")
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(root))
    monkeypatch.setattr(C, "IDENTITY_R4_DIR", r4)
    monkeypatch.setattr(C, "FSDS_DIR", tmp_path / "fsds")
    monkeypatch.setattr(C, "COMPANYFACTS_DIR", tmp_path / "cf")
    return root


def _backfill(root: Path) -> tuple[dict, list]:
    from atx_db.alpha_panel import identity_backfill

    receipt = identity_backfill.build()
    rows = pq.read_table(root / "identity" / "links_combined.parquet").to_pylist()
    return receipt, rows


def test_s2_off_is_unchanged_and_writes_backfill_days(backfill_lake, monkeypatch):
    monkeypatch.delenv("ATX_A0_S2", raising=False)
    receipt, rows = _backfill(backfill_lake)
    manifest = json.loads((backfill_lake / "identity" / "backfill_manifest.json").read_text(encoding="utf-8"))
    assert set(manifest) == {"rule", "candidates", "name_tier", "backfill_rows", "combined_rows", "line_days_by_year"}
    assert manifest["backfill_rows"] == 1 and manifest["candidates"] == [1, 1, 1]
    assert {r["tier"] for r in rows} == {"high", "backfill"}
    monkeypatch.setenv("ATX_A0_S2", "0")
    receipt_zero, rows_zero = _backfill(backfill_lake)
    assert receipt_zero == receipt and rows_zero == rows


def test_s2_on_gives_zero_backfill_rows_and_records_the_switch(backfill_lake, monkeypatch):
    monkeypatch.setenv("ATX_A0_S2", "1")
    _, rows = _backfill(backfill_lake)
    manifest = json.loads((backfill_lake / "identity" / "backfill_manifest.json").read_text(encoding="utf-8"))
    assert manifest["backfill_rows"] == 0 and manifest["candidates"] == [0, 0, 0]
    assert manifest["a0_s2"] is True
    assert {r["tier"] for r in rows} == {"high"}
    assert pq.read_table(backfill_lake / "identity" / "links_backfill.parquet").num_rows == 0
    monkeypatch.setenv("ATX_A0_S2", "on")
    with pytest.raises(ValueError, match="ATX_A0_S2"):
        _backfill(backfill_lake)


# ---------------------------------------------------------------------------------------------
# S3 + S4: the stage runner, one process per stage


def _r4_fixture(r4: Path) -> None:
    export = r4 / "phases" / "export-001"
    _copy("""SELECT 101::BIGINT AS perm_security_id, 11::BIGINT AS perm_company_id, '1' AS cik,
                    DATE '2015-01-02' AS link_start, CAST(NULL AS DATE) AS link_end, 'strict_dated' AS link_basis,
                    'P' AS link_primary, 'high' AS tier, TIMESTAMP '2015-01-01 00:00' AS available_at,
                    CAST(NULL AS TIMESTAMP) AS valid_until""", export / "security_company_links.part-0000.parquet")
    _copy("SELECT 101::BIGINT AS perm_security_id, 'TBLTICKERHISTORY-101' AS security_id",
          export / "security_permanent_ids.part-0000.parquet")
    (r4 / "manifest.json").write_text("{}", encoding="utf-8")


def test_stage_runner_redirects_r4_and_records_failing_validation_without_exiting(tmp_path):
    root, r4 = tmp_path / "P_A", tmp_path / "R_A"
    _r4_fixture(r4)
    _copy("SELECT DATE '2015-01-02' AS session_date UNION ALL SELECT DATE '2015-01-05'", root / "calendar.parquet")
    env = {k: v for k, v in os.environ.items() if not k.startswith("ATX_")}
    env["PYTHONPATH"] = str(SRC)
    proc = subprocess.run([sys.executable, "-m", "atx_db.alpha_panel.a0_check", "stage", "--root", str(root),
                           "--r4", str(r4), "--stage", "identity"], capture_output=True, text=True, env=env,
                          timeout=120)
    assert proc.returncode == 0, proc.stderr
    manifest = json.loads((root / "identity" / "manifest.json").read_text(encoding="utf-8"))
    validation = manifest["validation"]
    assert set(validation) == {"seal", "expected", "got", "pass"}
    assert validation["pass"] is False and validation["seal"] == "2025-01-01"
    assert validation["expected"]["rows"] == 6780 and validation["got"]["rows"] == 1
    assert Path(manifest["sources"]["security_company_links"]["path"]).parent == r4 / "phases" / "export-001"
    assert pq.read_table(root / "identity" / "links.parquet").num_rows == 1
    assert "validation False" in proc.stdout


def test_stage_runner_refuses_published_root_and_late_patch(tmp_path, monkeypatch):
    with pytest.raises(ValueError, match="published"):
        a0.check_arm_root(C.DEFAULT_ROOT)
    with pytest.raises(ValueError, match="published"):
        a0.check_arm_root(C.DEFAULT_ROOT / "identity")
    with pytest.raises(ValueError, match="published"):
        a0.check_arm_root(C.DEFAULT_ROOT.parent)
    monkeypatch.setitem(sys.modules, "atx_db.alpha_panel.identity_links", types.ModuleType("stale"))
    with pytest.raises(RuntimeError, match="imported before"):
        a0.run_stage(tmp_path / "P", tmp_path / "R", "identity")
    with pytest.raises(ValueError, match="unknown stage"):
        a0.run_stage(tmp_path / "P", tmp_path / "R", "prices")


def test_restore_data_paths_rebases_every_package_root_constant(monkeypatch):
    fake = Path("C:/elsewhere/export/atx-db")
    for name in a0.DATA_CONSTANTS:
        monkeypatch.setattr(C, name, fake / getattr(C, name).relative_to(C.PACKAGE_ROOT))
    monkeypatch.setattr(C, "PACKAGE_ROOT", fake)
    real = Path("C:/atx/atx-db/data")
    restored = a0.restore_data_paths(real)
    assert real / "alpha_panel" / "v1" == C.DEFAULT_ROOT
    assert real / "staging" / "fsds-v2" == C.FSDS_DIR
    assert real / "staging" / "companyfacts" / "ee099c7394a357f1" == C.COMPANYFACTS_DIR
    assert real / "research" / "identity_rehearsal" / "session8-phased-r4" == C.IDENTITY_R4_DIR
    assert set(restored) == set(a0.DATA_CONSTANTS)


# ---------------------------------------------------------------------------------------------
# Frozen code export


def test_frozen_export_assertion_rejects_atx_db_from_outside_the_export(tmp_path):
    with pytest.raises(RuntimeError, match="outside the frozen export"):
        a0.assert_frozen_code(tmp_path / "code" / "abc1234")
    assert a0.assert_frozen_code(SRC.parents[1]) == Path(atx_db.__file__).resolve()


# ---------------------------------------------------------------------------------------------
# run-arms plan


def test_plan_runs_the_request_procedure_serially_under_the_guard(tmp_path):
    base = tmp_path / "a0"
    inputs = {"receipt": str(base / "ri2_a.recovered.json"), "ops_manifest": str(tmp_path / "ops.json"),
              "document_review": str(tmp_path / "review.json")}
    steps = a0.plan_steps(base, "0123456789abcdef0123456789abcdef01234567", data_root=tmp_path / "data",
                          inputs=inputs)
    ids = [s["id"] for s in steps]
    assert ids[0] == "preflight"
    prepare = ["vendor", "facts", "reconstruct"]
    phases = ["symbols", "candidates", "uncertainty", "links", "names", "export"]
    stages = ["identity", "backfill", "names", "combine", "identity_table"]
    expected = ["preflight"]
    for arm in ("A", "B2"):
        expected += [f"W_{arm}.prepare.{s}" for s in prepare] + [f"R_{arm}.rehearsal.{p}" for p in phases]
    for arm in ("A", "B1", "B2", "B12"):
        expected += [f"P_{arm}.seed"] + [f"P_{arm}.stage.{s}" for s in stages] + [f"P_{arm}.unseed"]
    assert ids == expected
    by_id = {s["id"]: s for s in steps}
    assert by_id["W_A.prepare.reconstruct"]["env"]["ATX_A0_S1"] == "0"
    assert by_id["W_B2.prepare.vendor"]["env"]["ATX_A0_S1"] == "1"
    for arm, s2 in (("A", "0"), ("B1", "1"), ("B2", "0"), ("B12", "1")):
        assert by_id[f"P_{arm}.stage.backfill"]["env"]["ATX_A0_S2"] == s2
    assert by_id["P_B1.stage.identity"]["argv"][by_id["P_B1.stage.identity"]["argv"].index("--r4") + 1] == str(base / "R_A")
    assert by_id["P_B12.stage.combine"]["argv"][by_id["P_B12.stage.combine"]["argv"].index("--r4") + 1] == str(base / "R_B2")
    code = base / "code" / "0123456"
    for step in steps:
        assert step["cap"] <= 1.0
        assert step["argv"][step["argv"].index("--code-root") + 1] == str(code)
        command = a0.guard_command(step, receipt=tmp_path / "r.json", stdout=tmp_path / "o", stderr=tmp_path / "e",
                                   python=Path(sys.executable), guard=tmp_path / "guard.py")
        head = command[:command.index("--")]
        for flag in ("--heavy", "--quiet"):
            assert flag in head
        assert head[head.index("--wait-minutes") + 1] == "90"
        assert head[head.index("--disk-path") + 1] == "C:/"
        assert head[head.index("--min-free-disk-gb") + 1] == "25"
        assert float(head[head.index("--job-gb") + 1]) == step["cap"]
    rehearsal = by_id["R_B2.rehearsal.export"]["argv"]
    assert rehearsal[rehearsal.index("--out") + 1] == str(base / "R_B2")
    assert rehearsal[rehearsal.index("--work") + 1] == str(base / "W_B2")
    assert rehearsal[rehearsal.index("--document-review") + 1] == inputs["document_review"]
    assert rehearsal[rehearsal.index("--ops-manifest") + 1] == inputs["ops_manifest"]
    prepare = by_id["W_B2.prepare.facts"]["argv"]
    assert prepare[prepare.index("--receipt") + 1] == inputs["receipt"]
    assert prepare[prepare.index("--") + 1:prepare.index("--") + 3] == ["atx_db.identity_links", "prepare"]


def _step_record(base: Path, step_id: str, *codes: int) -> None:
    (base / "steps").mkdir(parents=True, exist_ok=True)
    (base / "steps" / f"{step_id}.json").write_text(json.dumps(
        {"id": step_id, "attempts": [{"exit_code": c} for c in codes]}), encoding="utf-8")


def test_failed_first_export_moves_the_arm_to_a_new_rehearsal_root(tmp_path):
    base = tmp_path / "a0"
    assert a0.rehearsal_root(base, "A") == base / "R_A"
    _step_record(base, "R_A.rehearsal.export", 78)  # guard never admitted it: nothing ran
    assert a0.rehearsal_root(base, "A") == base / "R_A"
    _step_record(base, "R_A.rehearsal.export", 78, 1)
    assert a0.rehearsal_root(base, "A") == base / "R_A-2"
    _step_record(base, "R_A-2.rehearsal.export", 0)
    assert a0.rehearsal_root(base, "A") == base / "R_A-2"
    _step_record(base, "R_B2.rehearsal.export", 137)
    assert a0.rehearsal_root(base, "B2") == base / "R_B2-2"
    steps = a0.plan_steps(base, "0" * 40, data_root=tmp_path / "data",
                          inputs={"receipt": "r", "ops_manifest": "o", "document_review": "d"})
    by_id = {s["id"]: s for s in steps}
    assert "R_A-2.rehearsal.symbols" in by_id and "R_A.rehearsal.symbols" not in by_id
    argv = by_id["P_B1.stage.identity"]["argv"]
    assert argv[argv.index("--r4") + 1] == str(base / "R_A-2")


def test_dry_run_prints_the_plan_but_the_real_run_refuses_incomplete_recovery(tmp_path, capsys, monkeypatch):
    base = tmp_path / "a0"
    base.mkdir()
    (base / "a0-inputs.json").write_text(json.dumps({
        "receipt": {"path": "r"}, "ops_manifest": {"path": "o"}, "document_review": {"path": "d"},
        "documents": {"status": "stopped_hash_mismatch", "mismatched_urls": ["https://www.sec.gov/x.htm"]}}),
        encoding="utf-8")
    monkeypatch.delenv("ATX_GUARD_JOB", raising=False)
    kwargs = {"data_root": tmp_path / "data", "python": Path(sys.executable), "guard": tmp_path / "guard.py"}
    assert a0.run_arms(base, "HEAD", dry_run=True, **kwargs) == 0
    printed = capsys.readouterr().out
    assert "BLOCKED" in printed and "stopped_hash_mismatch" in printed and "https://www.sec.gov/x.htm" in printed
    assert "P_B12.unseed" in printed and "--heavy" in printed
    with pytest.raises(SystemExit, match="did not complete"):
        a0.run_arms(base, "HEAD", dry_run=False, **kwargs)
    assert sorted(p.name for p in base.iterdir()) == ["a0-inputs.json"]


def test_export_phase_refuses_an_unsealed_first_attempt(tmp_path):
    out = tmp_path / "R_A"
    (out / "phases" / "export-001").mkdir(parents=True)
    argv = ["rehearsal", "--work", str(tmp_path / "W_A"), "--ops-manifest", "o", "--out", str(out), "--phase", "export"]
    with pytest.raises(RuntimeError, match="export-001"):
        a0.run_module("atx_db.identity_links", argv)
    (out / "phases" / "export.json").write_text(json.dumps(
        {"launch": str(Path("phases") / "export-002" / "launch.json")}), encoding="utf-8")
    assert not a0._export_sealed(out)
    (out / "phases" / "export.json").write_text(json.dumps(
        {"launch": str(Path("phases") / "export-001" / "launch.json")}), encoding="utf-8")
    assert a0._export_sealed(out)
    with pytest.raises(ValueError, match="exec runs only"):
        a0.run_module("atx_db.alpha_panel.identity_links", ["build"])


def test_s4_gate_sees_one_pass_and_the_manifest_keeps_the_true_result():
    gate = a0._GateOnce({"seal": "2025-01-01", "got": {"rows": 1}, "expected": {"rows": 2}, "pass": False})
    assert gate["pass"] is True
    assert gate["pass"] is False
    assert json.loads(json.dumps({"validation": gate}, indent=2, sort_keys=True))["validation"]["pass"] is False


# ---------------------------------------------------------------------------------------------
# report: row 11 and the step-3 sanity checks


def _evidence(work: Path, symbol_items: int) -> None:
    rows = [{"evidence_id": "e1", "value_json": json.dumps({"items": [{"kind": "shares_outstanding"}]
                                                            + [{"kind": "symbol_snapshot_match"}] * symbol_items})},
            {"evidence_id": "e2-R", "value_json": json.dumps({"rejection_reason": "x"})}]
    (work / "evidence").mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows), work / "evidence" / "part-0000.parquet")
    (work / "preparation.json").write_text(json.dumps({
        "steps": {"reconstruct": {"files": [{"file": "evidence/part-0000.parquet"}]}}}), encoding="utf-8")


def _arm_manifests(root: Path, *, backfill_rows: int, rows: int, validation: bool = True, s2: bool = False) -> None:
    (root / "identity").mkdir(parents=True, exist_ok=True)
    got = {"rows": rows}
    (root / "identity" / "manifest.json").write_text(json.dumps({"validation": {
        "seal": "2025-01-01", "expected": {"rows": 6780}, "got": got, "pass": rows == 6780}} if validation else {}),
        encoding="utf-8")
    (root / "identity" / "backfill_manifest.json").write_text(json.dumps(
        {"backfill_rows": backfill_rows, "name_tier": True, **({"a0_s2": True} if s2 else {})}), encoding="utf-8")


def _report_lake(base: Path) -> None:
    _evidence(base / "W_A", 2)
    _evidence(base / "W_B2", 0)
    for work, s1 in (("W_A", False), ("W_B2", True)):
        (base / work / "preparation.json").write_text(json.dumps(
            {"pin": {"version": "v", **({"a0_s1": True} if s1 else {})},
             "steps": {"reconstruct": {"files": [{"file": "evidence/part-0000.parquet"}]}}}), encoding="utf-8")
    for arm, bf, rows, s2 in (("A", 10, 6780, False), ("B1", 0, 6780, True), ("B2", 7, 6700, False),
                              ("B12", 0, 6700, True)):
        _arm_manifests(base / f"P_{arm}", backfill_rows=bf, rows=rows, s2=s2)
    for arm in ("A", "B2"):
        phases = base / f"R_{arm}" / "phases"
        (phases / "export-001").mkdir(parents=True)
        (phases / "export.json").write_text(json.dumps(
            {"launch": str(Path("phases") / "export-001" / "launch.json")}), encoding="utf-8")


def test_report_assembles_row_11_and_sanity_lines_from_fixture_manifests(tmp_path):
    base = tmp_path / "a0"
    _report_lake(base)
    facts = a0.collect_arm_facts(base)
    row = a0.row_11(facts)
    assert row["B1"] == {"backfill_rows": 0, "symbol_snapshot_match": 2, "name_tier": True,
                         "validation_got": {"rows": 6780}}
    assert row["B2"]["symbol_snapshot_match"] == 0 and row["B2"]["validation_got"] == {"rows": 6700}
    assert row["B12"]["backfill_rows"] == 0 and row["B12"]["symbol_snapshot_match"] == 0
    lines = a0.sanity_lines(a0.sanity_checks(base, facts))
    assert lines and all(line.startswith("PASS ") for line in lines), lines
    assert any("B12" in line and "backfill_rows" in line for line in lines)
    _arm_manifests(base / "P_B1", backfill_rows=3, rows=6780, s2=True)
    _arm_manifests(base / "P_B2", backfill_rows=7, rows=6700, validation=False)
    lines = a0.sanity_lines(a0.sanity_checks(base, a0.collect_arm_facts(base)))
    failed = [line for line in lines if line.startswith("FAIL ")]
    assert len(failed) == 2, failed
    assert any("B1" in line and "backfill_rows = 0" in line for line in failed)
    assert any("B2" in line and "validation" in line for line in failed)
    # an arm run with the wrong switch binding is a FAIL too, as is an export sealed at a retry
    (base / "W_B2" / "preparation.json").write_text(json.dumps({"pin": {}, "steps": {}}), encoding="utf-8")
    _arm_manifests(base / "P_B12", backfill_rows=0, rows=6700)
    (base / "R_A" / "phases" / "export.json").write_text(json.dumps(
        {"launch": str(Path("phases") / "export-002" / "launch.json")}), encoding="utf-8")
    failed = [line for line in a0.sanity_lines(a0.sanity_checks(base, a0.collect_arm_facts(base)))
              if line.startswith("FAIL ")]
    assert any("W_B2" in line and "a0_s1" in line for line in failed)
    assert any("B12" in line and "a0_s2" in line for line in failed)
    assert any("R_A" in line and "export" in line for line in failed)


def test_extended_window_ends_at_the_pinned_calendar_and_marks_the_partial_year(tmp_path):
    calendar = tmp_path / "calendar.parquet"
    _copy("SELECT DATE '2019-12-31' AS session_date UNION ALL SELECT DATE '2026-09-18'", calendar)
    con = duckdb.connect()
    try:
        latest = a0.pinned_latest_session(calendar, _sha(calendar), con)
        with pytest.raises(ValueError, match="seed-time"):
            a0.pinned_latest_session(calendar, "0" * 64, con)
    finally:
        con.close()
    assert latest == dt.date(2026, 9, 18) and a0.partial_year(latest) and not a0.partial_year(dt.date(2025, 12, 31))
    clock = dt.datetime(2026, 9, 20, 0, 6, 20)
    window = a0.window_for("2020-latest", latest, clock)
    assert window.first_session == dt.date(2020, 1, 1) and window.last_session == latest
    assert window.cutoff == dt.date(2026, 9, 19) and window.panel_from_year == 2020 and window.observed_at == clock
    assert [b[0] for b in window.row_7_buckets] == [str(y) for y in range(2020, 2027)]
    original = a0.window_for("original", latest, clock)
    assert original.last_session == dt.date(2023, 12, 29) and original.observed_at is None
    with pytest.raises(ValueError, match="unknown window"):
        a0.window_for("2024", latest)


def test_zero_gate_and_pit_discovery(tmp_path):
    zero = {"rows_1_4": {"1_a_only": 0, "2_cik": 0}, "row_7": {"2020": {"a_only": 0, "b_only": 0, "a_total": 9}},
            "row_8": {"a_only": 0, "b_only": 0}}
    assert a0.zero_gate({"B12 vs A": zero})["pass"]
    moved = {**zero, "row_8": {"a_only": 0, "b_only": 2}}
    assert a0.zero_gate({"B12 vs A": moved}) == {"pass": False, "counts": {"rows_1_4": 0, "row_7": 0, "row_8": 2}}
    roots = [tmp_path / f"P_{arm}" for arm in ("A", "B1")]
    for root in roots:
        (root / "export" / "identity-bridge-v2-pit").mkdir(parents=True)
        (root / "export" / "identity-bridge-v2-all").mkdir(parents=True)
    assert a0.discover_pit(roots) == "identity-bridge-v2-pit"
    (roots[1] / "export" / "identity-bridge-v3-pit").mkdir()
    with pytest.raises(ValueError, match="--pit-export"):
        a0.discover_pit(roots)


def test_symbol_snapshot_items_counts_items_not_rows(tmp_path):
    _evidence(tmp_path / "W", 3)
    assert a0.symbol_snapshot_items(tmp_path / "W") == 3
    with pytest.raises(FileNotFoundError):
        a0.symbol_snapshot_items(tmp_path / "missing")


# ---------------------------------------------------------------------------------------------
# recover-inputs: pinned review bodies, verified, stop on the first mismatch


class FakeFetcher:
    def __init__(self, cache_dir: Path, bodies: dict[str, bytes]):
        self.cache_dir, self.bodies, self.requests = cache_dir, bodies, []

    def cache_path(self, url: str) -> Path:
        return self.cache_dir / "Archives" / Path(*url.rsplit("/", 2)[-2:])

    def fetch(self, url: str):
        self.requests.append(url)
        path = self.cache_path(url)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self.bodies[url])
        return types.SimpleNamespace(status=200, path=path, sha256=hashlib.sha256(self.bodies[url]).hexdigest())


def _review(tmp_path: Path, fetcher: FakeFetcher, pinned: dict[str, bytes]) -> Path:
    assertions = [{"document_url": url, "document_path": str(fetcher.cache_path(url)),
                   "document_sha256": hashlib.sha256(body).hexdigest()} for url, body in pinned.items()]
    review = tmp_path / "review.json"
    review.write_text(json.dumps({"version": "identity_dated_document_review_v1", "assertions": assertions}),
                      encoding="utf-8")
    return review


def test_recover_documents_verifies_every_pinned_body(tmp_path):
    pinned = {f"https://www.sec.gov/Archives/edgar/data/1/{n:03d}/d.htm": f"body {n}".encode() for n in range(3)}
    pinned["https://www.sec.gov/Archives/edgar/data/1/999/big.htm"] = b""
    fetcher = FakeFetcher(tmp_path / "cache", dict(pinned))
    review = _review(tmp_path, fetcher, pinned)
    build_pins = {str(fetcher.cache_path(u).resolve()).upper(): hashlib.sha256(b).hexdigest() for u, b in pinned.items()}
    result = a0.recover_documents(review, fetcher, pinned=build_pins)
    assert result["documents"] == 4 and result["matched"] == 4 and result["mismatched_urls"] == []
    assert result["empty_bodies"] == 1 and result["status"] == "complete"
    with pytest.raises(ValueError, match="differ"):
        a0.recover_documents(review, fetcher, pinned={**build_pins, "C:\\other.htm": "0" * 64})
    assert len(fetcher.requests) == 4


def test_recover_documents_stops_on_a_mismatch_and_lists_the_url(tmp_path):
    pinned = {f"https://www.sec.gov/Archives/edgar/data/1/{n:03d}/d.htm": f"body {n}".encode() for n in range(3)}
    served = dict(pinned)
    bad = sorted(pinned)[1]
    served[bad] = b"changed"
    fetcher = FakeFetcher(tmp_path / "cache", served)
    result = a0.recover_documents(_review(tmp_path, fetcher, pinned), fetcher)
    assert result["status"] == "stopped_hash_mismatch"
    assert result["mismatched_urls"] == [bad]
    assert fetcher.requests == sorted(pinned)[:2]
    assert result["not_attempted"] == 1


def test_recover_documents_refuses_a_pinned_path_outside_the_fetcher_layout(tmp_path):
    url = "https://www.sec.gov/Archives/edgar/data/1/001/d.htm"
    fetcher = FakeFetcher(tmp_path / "cache", {url: b"x"})
    review = tmp_path / "review.json"
    review.write_text(json.dumps({"version": "identity_dated_document_review_v1", "assertions": [
        {"document_url": url, "document_path": str(tmp_path / "other" / "d.htm"),
         "document_sha256": hashlib.sha256(b"x").hexdigest()}]}), encoding="utf-8")
    with pytest.raises(ValueError, match="layout"):
        a0.recover_documents(review, fetcher)
    assert fetcher.requests == []


def test_recovered_receipt_copies_published_inputs_verbatim(tmp_path):
    inputs = {"ticker_history": {"path": "C:\\x\\TickerHistory3.parquet", "sha256": "0" * 64,
                                 "received_at": "2026-09-20T16:06:08.749429", "receipt_basis": "file_mtime"}}
    build = tmp_path / "build.json"
    build.write_text(json.dumps({"pin": {"inputs": inputs}}), encoding="utf-8")
    dest = tmp_path / "ri2_a.recovered.json"
    payload = a0.write_recovered_receipt(build, dest)
    assert json.loads(dest.read_text(encoding="utf-8")) == payload
    assert payload["inputs"] == inputs and "recovery" in payload
    assert a0.write_recovered_receipt(build, dest) == payload
    build.write_text(json.dumps({"pin": {"inputs": {**inputs, "extra": {}}}}), encoding="utf-8")
    with pytest.raises(ValueError, match="differs"):
        a0.write_recovered_receipt(build, dest)


# ---------------------------------------------------------------------------------------------
# frozen export, arm seeds and the whole report on tiny fixtures


def _git_repo(tmp_path: Path) -> tuple[Path, str]:
    repo = tmp_path / "repo"
    for rel in a0.IDENTITY_MODULES:
        path = repo / "atx-db" / "src" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"# {rel}\n", encoding="utf-8")
    (repo / "atx-db" / "data").mkdir(parents=True)
    (repo / "atx-db" / "data" / "lake.txt").write_text("not exported", encoding="utf-8")
    git = ["git", "-C", str(repo)]
    subprocess.run([*git, "init", "-q"], check=True)
    subprocess.run([*git, "add", "--", "atx-db"], check=True)
    subprocess.run([*git, "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false",
                    "commit", "-q", "-m", "fixture"], check=True)
    sha = subprocess.run([*git, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    return repo, sha


def test_frozen_export_is_a_plain_archive_of_src_pinned_in_arms_json(tmp_path):
    repo, sha = _git_repo(tmp_path)
    base = tmp_path / "a0"
    code = a0.ensure_export(repo, sha, base, {"arms": {}})
    assert code == base / "code" / sha[:7]
    assert (code / "atx-db" / "src" / "atx_db" / "alpha_panel" / "a0_check.py").is_file()
    assert not (code / "atx-db" / "data").exists()
    for item in code.rglob("*"):
        assert not item.is_symlink() and not item.is_junction()
    arms = json.loads((base / "arms.json").read_text(encoding="utf-8"))
    assert arms["commit"] == sha and set(arms["modules"]) == set(a0.IDENTITY_MODULES)
    assert a0.ensure_export(repo, sha, base, {"arms": {}}) == code
    (code / "atx-db" / "src" / "atx_db" / "identity_links.py").write_text("# edited\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="changed"):
        a0.ensure_export(repo, sha, base, {"arms": {}})
    with pytest.raises(SystemExit, match="belongs to commit"):
        a0.ensure_export(repo, "f" * 40, base, {"arms": {}})


@pytest.fixture
def fake_package(tmp_path, monkeypatch):
    """``alpha_panel.common`` as if loaded from an export, with a synthetic real data root."""
    fake, data = tmp_path / "export" / "atx-db", tmp_path / "data"
    for name in a0.DATA_CONSTANTS:
        monkeypatch.setattr(C, name, fake / Path(getattr(C, name)).relative_to(C.PACKAGE_ROOT))
    monkeypatch.setattr(C, "PACKAGE_ROOT", fake)
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "unused"))
    return data


def test_seed_copies_published_inputs_and_unseed_removes_only_copies_and_scratch(fake_package, tmp_path):
    published = fake_package / "alpha_panel" / "v1"
    _copy("SELECT DATE '2026-09-17' AS session_date UNION ALL SELECT DATE '2026-09-18'", published / "calendar.parquet")
    _copy("SELECT 1::BIGINT AS security_id, DATE '2026-09-18' AS session_date",
          published / "prices" / "year=2026" / "prices.parquet")
    _copy("SELECT 1 AS x", published / "short_interest" / "si.parquet")
    _copy("SELECT 1 AS x", published / "_tmp" / "panel_core" / "bucket_00.parquet")
    _copy("SELECT 1::BIGINT AS security_id, DATE '2026-09-17' AS session_date, TRUE AS member",
          published / "_tmp" / "panel_member" / "year=2026.parquet")
    (published / "prices" / "manifest.json").write_text("{}", encoding="utf-8")
    before = {p: (p.stat().st_mtime_ns, _sha(p)) for p in published.rglob("*") if p.is_file()}
    root, record = tmp_path / "a0" / "P_A", tmp_path / "a0" / "seeds" / "P_A.json"
    seeded = a0.seed_arm(root, record, data_root=fake_package)
    assert published == C.DEFAULT_ROOT
    seeds = ["_tmp/panel_core/bucket_00.parquet", "_tmp/panel_member/year=2026.parquet", "calendar.parquet",
             "prices/year=2026/prices.parquet", "short_interest/si.parquet"]
    assert sorted(f["path"] for f in seeded["files"]) == seeds
    assert seeded["coverage"]["calendar_last_session"] == "2026-09-18"
    assert seeded["coverage"]["member_last_session"] == "2026-09-17"
    assert a0.seed_arm(root, record, data_root=fake_package)["reused"]
    with pytest.raises(ValueError, match="published"):
        a0.seed_arm(published, tmp_path / "x.json", data_root=fake_package)
    (root / "identity").mkdir()
    (root / "identity" / "links.parquet").write_bytes(b"stage output")
    (root / "_tmp" / "identity_backfill.duckdb").write_bytes(b"scratch")
    (root / "_tmp" / "spill-123").mkdir()
    removed = {r["path"] for r in a0.unseed_arm(root, record, data_root=fake_package)["removed"]}
    assert removed >= {*seeds, "_tmp/identity_backfill.duckdb", "_tmp/spill-123"}
    assert sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()) == ["identity/links.parquet"]
    assert {p: (p.stat().st_mtime_ns, _sha(p)) for p in published.rglob("*") if p.is_file()} == before
    assert (record.parent / "P_A.unseeded.json").is_file()


def _pit(dest: Path, extra: str = "") -> None:
    _copy("""SELECT * FROM (VALUES (101::BIGINT, 1::BIGINT, DATE '2017-10-02', DATE '2026-09-18',
             TIMESTAMP '2017-10-01 00:00', 'P', 'strict_dated', 'common')""" + extra + """)
             t(sr_id, cik, "start", end_incl, available_at, "primary", basis, class_status)""", dest)


def _full_report_lake(tmp_path: Path) -> tuple[Path, Path]:
    data, base = tmp_path / "data", tmp_path / "a0"
    published, r4 = data / "alpha_panel" / "v1", data / "research" / "identity_rehearsal" / "session8-phased-r4"
    _report_lake(base)  # backfill manifests, evidence, preparation pins, export seals
    late = (", (202::BIGINT, 2::BIGINT, DATE '2022-01-03', DATE '2026-09-18', TIMESTAMP '2022-01-02 00:00', 'P', "
            "'strict_dated', 'common')")
    _copy("SELECT CAST(d AS DATE) AS session_date FROM range(DATE '2017-10-02', DATE '2026-09-19', INTERVAL 1 DAY) t(d) "
          "WHERE isodow(d) <= 5", published / "calendar.parquet")
    _copy(f"SELECT 101::BIGINT AS security_id, session_date, TRUE AS member FROM read_parquet("
          f"'{(published / 'calendar.parquet').as_posix()}') WHERE year(session_date) >= 2018",
          published / "_tmp" / "panel_member" / "year=all.parquet")
    r4_links = """SELECT 101::BIGINT AS perm_security_id, 11::BIGINT AS perm_company_id, '1' AS cik,
        DATE '2017-10-02' AS link_start, CAST(NULL AS DATE) AS link_end, 'current_ticker_verified' AS link_basis,
        'P' AS link_primary, 'high' AS tier, TIMESTAMP '2026-09-20 22:00' AS available_at,
        CAST(NULL AS VARCHAR) AS evidence_ids, TIMESTAMP '2026-09-21 01:00' AS created_at,
        CAST(NULL AS TIMESTAMP) AS valid_until, 'common' AS class_status"""
    rel = Path("phases") / "export-001" / "security_company_links.part-0000.parquet"
    for root in (r4, base / "R_A", base / "R_B2"):
        _copy(r4_links, root / rel)
        files = [{"file": rel.as_posix(), "table": "security_company_links", "sha256": _sha(root / rel)}]
        (root / "phases" / "export.json").write_text(json.dumps({"files": files, "launch": str(
            Path("phases") / "export-001" / "launch.json")}), encoding="utf-8")
        (root / "manifest.json").write_text(json.dumps({"files": files, "snapshot_date": "2026-09-20",
            "code_sha256": "0" * 64, "code_files": {"identity_links.py": "0" * 64}}), encoding="utf-8")
    code = {"identity_table.py": {"sha256": "1" * 64, "sha256_lf": "2" * 64}, "git_head": None}
    for root, extra in ((published, ""), (base / "P_A", ""), (base / "P_B1", ""), (base / "P_B2", late),
                        (base / "P_B12", late)):
        pit = root / "export" / "identity-bridge-v2-pit"
        _pit(pit / "links.parquet", extra)
        (pit / "manifest.json").write_text(json.dumps(
            {"files": {"links.parquet": {"sha256": _sha(pit / "links.parquet")}}, "code": code}), encoding="utf-8")
        ident = root / "identity"
        _copy("""SELECT 101::BIGINT AS security_id, 1::BIGINT AS cik, DATE '2017-10-02' AS valid_from,
                 DATE '2026-09-18' AS valid_to, TIMESTAMP '2017-10-01' AS available_at,
                 TIMESTAMP '2017-10-01' AS evidence_at, 'strict' AS link_tier""", ident / "link_table.parquet")
        (ident / "link_table_manifest.json").write_text(json.dumps(
            {"files": {"link_table.parquet": {"sha256": _sha(ident / "link_table.parquet")}}, "code": code}),
            encoding="utf-8")
        _copy("""SELECT 101::BIGINT AS security_id, 1::BIGINT AS cik, DATE '2017-10-02' AS "start",
                 DATE '2026-09-18' AS end_incl, 'P' AS "primary", 'high' AS tier, 'strict_dated' AS basis,
                 TIMESTAMP '2017-10-01' AS available_at""", ident / "links.parquet")
        (ident / "manifest.json").write_text(json.dumps({"code_version": "identity-links-v1", "sources": {},
            "validation": {"seal": "2025-01-01", "expected": {"rows": 1}, "got": {"rows": 1}, "pass": True}}),
            encoding="utf-8")
    (base / "seeds").mkdir()
    for arm in ("A", "B1", "B2", "B12"):
        (base / f"P_{arm}" / "_a0").mkdir()
        (base / f"P_{arm}" / "_a0" / "stage-identity_table.json").write_text(json.dumps(
            {"coverage": {"calendar_last_session": "2026-09-18"}}), encoding="utf-8")
        files = [{"path": p.relative_to(published).as_posix(), "source": str(p), "sha256": _sha(p)}
                 for p in (published / "calendar.parquet", published / "_tmp" / "panel_member" / "year=all.parquet")]
        (base / "seeds" / f"P_{arm}.json").write_text(json.dumps(
            {"files": files, "coverage": {"calendar_last_session": "2026-09-18"}}), encoding="utf-8")
    receipt = base / "ri2_a.recovered.json"
    receipt.write_text(json.dumps({"inputs": {"company_tickers": {"received_at": "2026-09-20T00:06:20.722492"}}}),
                       encoding="utf-8")
    (base / "arms.json").write_text(json.dumps({"commit": "0" * 40, "inputs": {"receipt": str(receipt)}}),
                                    encoding="utf-8")
    return base, data


def _weekdays(first: dt.date, last: dt.date) -> int:
    return sum(1 for n in range((last - first).days + 1) if (first + dt.timedelta(days=n)).weekday() < 5)


def test_report_writes_both_windows_from_tiny_arms(tmp_path):
    base, data = _full_report_lake(tmp_path)
    out = tmp_path / "report"
    results = a0.report(base, data_root=data, out=out)
    assert (out / "a0-results.json").is_file() and (out / "a0-results.md").is_file()
    assert not (out / "a0-report.duckdb").exists() and not (out / "a0-report.spill").exists()
    assert results["coverage"]["pinned_calendar"]["last_session"] == "2026-09-18"
    assert results["coverage"]["pinned_calendar"]["latest_year_partial"] is True
    assert results["coverage"]["observation_clocks"] == {"s1": "2026-09-20 00:06:20.722492",
                                                         "s2": "2026-09-20 22:00:00"}
    assert all(results["d0"][kind]["d0_pass"] for kind in ("r4", "pit", "link_table"))
    assert results["d0"]["stage_i_validation"] == {"pass": True}
    assert results["provenance"]["unmatched"] and "No producing commit" in \
        results["provenance"]["producing_commit_statement"]
    original, extended = results["windows"]["original"], results["windows"]["2020-latest"]
    assert original["window"]["last_session"] == "2023-12-29"
    assert extended["window"]["start_session"] == "2020-01-01" and extended["window"]["last_session"] == "2026-09-18"
    b12 = original["rows"]["B12 vs A"]
    assert b12["rows_1_4"]["1_b_only"] == _weekdays(dt.date(2022, 1, 3), dt.date(2023, 12, 29))
    assert original["rows"]["B1 vs A"]["rows_1_4"]["1_b_only"] == 0
    assert original["zero_gate_b12_vs_a"]["pass"] is False
    ext = extended["rows"]["B12 vs A"]
    assert ext["rows_1_4"]["1_b_only"] == _weekdays(dt.date(2022, 1, 3), dt.date(2026, 9, 18))
    assert ext["rows_1_4_by_observation"]["before"]["1_b_only"] == ext["rows_1_4"]["1_b_only"]
    assert list(ext["row_7"]) == [str(y) for y in range(2020, 2027)]
    assert (out / "identifiers" / "2020-latest" / "B12-vs-B1" / "counts.json").is_file()
    assert all(line.startswith("PASS ") for line in results["sanity"]), results["sanity"]
    assert extended["b2_attribution_aid"]["stage_i_links"]["rows_in_a_only"] == 0
    text = (out / "a0-results.md").read_text(encoding="utf-8")
    assert "| # | count | B1 vs A | B2 vs A | B12 vs A | B12 vs B1 |" in text and "FAIL zero gate" in text
    results["baseline"] = "repinned"
    results["recovery"] = {"document_review": {"path": "new-review", "sha256": "new-hash"},
                           "repinning": {"original_review": {"path": "original-review", "sha256": "old-hash"},
                                         "note": "Historical-byte equivalence is not established"}}
    text = a0.markdown(results)
    assert "new-review" in text and "original-review" in text and "old-hash" in text and "new-hash" in text
    assert "Historical-byte equivalence is not established" in text
    with pytest.raises(SystemExit, match="not empty"):
        a0.report(base, data_root=data, out=out)


def test_repinned_review_preserves_events_and_rejects_content_changes(tmp_path):
    url = "https://www.sec.gov/Archives/edgar/data/1/001/d.htm"
    fetcher = FakeFetcher(tmp_path / "cache", {url: b"new delivery body"})
    original = _review(tmp_path, fetcher, {url: b"old delivery body"})
    payload = json.loads(original.read_text())
    payload["assertions"][0].update(candidate_events=[{"available_at": "2020-01-02", "evidence": "reviewed"}])
    original.write_text(json.dumps(payload))
    blob = original.read_bytes()
    fetch = fetcher.fetch
    def with_receipt(url):
        response = fetch(url)
        response.from_cache = False
        meta = {"status": 200, "sha256": response.sha256, "error": None, "fetched_at": "2026-10-05"}
        response.path.with_name(response.path.name + ".meta.json").write_text(json.dumps(meta))
        return response
    fetcher.fetch = with_receipt
    dest = tmp_path / "new-review.json"
    pinned = {payload["assertions"][0]["document_path"]: payload["assertions"][0]["document_sha256"]}
    mapping = a0.repin_documents(original, fetcher, dest, pinned)
    assert original.read_bytes() == blob
    assert mapping["documents"][0]["changed"]
    assert json.loads(dest.read_text())["assertions"][0]["candidate_events"] == payload["assertions"][0]["candidate_events"]
    assert a0.repin_documents(original, fetcher, dest, pinned) == mapping
    assert len(fetcher.requests) == 1
    amended = json.loads(dest.read_text())
    amended["assertions"][0]["candidate_events"][0]["available_at"] = "2019-01-01"
    dest.write_text(json.dumps(amended))
    mapping["review"]["sha256"] = hashlib.sha256(dest.read_bytes()).hexdigest()
    with pytest.raises(ValueError, match="events, clocks or evidence"):
        a0.verify_repinned_review(original, dest, mapping)
    dest.write_text(json.dumps(json.loads(blob)))
    with pytest.raises(ValueError, match="review changed"):
        a0.repin_documents(original, fetcher, dest, pinned)


def test_repinned_plan_uses_one_review_and_explicit_baseline(tmp_path):
    base = tmp_path / "base"
    inputs = {"receipt": "receipt", "ops_manifest": "ops", "document_review": "new-review"}
    steps = a0.plan_steps(base, "f" * 40, data_root=tmp_path / "data", inputs=inputs, baseline="repinned")
    assert steps[0]["argv"][-2:] == ["--baseline", "repinned"]
    reviews = [s["argv"][s["argv"].index("--document-review") + 1] for s in steps if "--document-review" in s["argv"]]
    assert len(reviews) == 12 and set(reviews) == {"new-review"}
    assert a0.baseline_inputs_path(base, "historical") != a0.baseline_inputs_path(base, "repinned")


def test_repin_accounts_for_oversized_and_stops_on_fetch_error(tmp_path):
    urls = [f"https://www.sec.gov/Archives/edgar/data/1/{n:03d}/d.htm" for n in range(2)]
    fetcher = FakeFetcher(tmp_path / "cache", {urls[0]: b"", urls[1]: b"body"})
    original = _review(tmp_path, fetcher, {urls[0]: b"", urls[1]: b"old"})
    pinned = {r["document_path"]: r["document_sha256"] for r in json.loads(original.read_text())["assertions"]}
    fetch = fetcher.fetch
    fail = True
    def with_receipt(url):
        response = fetch(url)
        response.from_cache = False
        error = "response_too_large" if url == urls[0] else ("network failure" if fail else None)
        response.path.with_name(response.path.name + ".meta.json").write_text(json.dumps({
            "status": 200, "sha256": response.sha256, "error": error}))
        return response
    fetcher.fetch = with_receipt
    dest = tmp_path / "new-review.json"
    with pytest.raises(ValueError, match="fetch failed"):
        a0.repin_documents(original, fetcher, dest, pinned)
    assert not dest.exists() and not dest.with_suffix(".mapping.json").exists()
    fail = False
    mapping = a0.repin_documents(original, fetcher, dest, pinned)
    assert mapping["documents"][0]["bytes"] == 0
    assert mapping["documents"][0]["fetch_receipt"]["error"] == "response_too_large"
    fetcher.cache_path(urls[1]).write_bytes(b"tampered")
    with pytest.raises(ValueError, match="delivery body changed"):
        a0.verify_repinned_review(original, dest, mapping)


def test_run_arms_refuses_baseline_selection_mismatch(tmp_path, monkeypatch):
    base = tmp_path / "base"
    base.mkdir()
    (base / "a0-inputs.repinned.json").write_text(json.dumps({"baseline": "historical"}))
    monkeypatch.setattr(a0, "_git", lambda *args: b"f" * 40)
    with pytest.raises(SystemExit, match="baseline differs"):
        a0.run_arms(base, "HEAD", data_root=tmp_path, python=Path(sys.executable), guard=tmp_path / "guard",
                    dry_run=False, baseline="repinned")


def test_report_compares_fixed_v4_to_explicit_original_v2_reference(tmp_path):
    base, data = _full_report_lake(tmp_path)
    for arm in a0.ARMS:
        export = base / f"P_{arm}" / "export"
        (export / "identity-bridge-v2-pit").rename(export / "identity-bridge-v4-pit")
    published = data / a0.PUBLISHED_ROOT
    # A newer published family must neither replace nor confuse the original reference.
    decoy = published / "export" / "identity-bridge-v3-pit"
    decoy.mkdir()
    (decoy / "manifest.json").write_text('{"unrelated": true}')
    results = a0.report(base, data_root=data, out=tmp_path / "report")
    assert results["pit_export"] == "identity-bridge-v4-pit"
    assert results["published_pit_export"] == "identity-bridge-v2-pit"
    assert results["d0"]["pit"]["d0_pass"]
    assert results["d0"]["published_pit_manifest_sha256"] == _sha(published / "export" / "identity-bridge-v2-pit" / "manifest.json")
    assert results["provenance"]["pit_export"]["manifest"] == str(published / "export" / "identity-bridge-v2-pit" / "manifest.json")
    assert all("identity-bridge-v4-pit" in path for path in results["pit_paths"]["arms"].values())
    assert "identity-bridge-v2-pit" in results["pit_paths"]["published_reference"]
    statement = results["provenance"]["producing_commit_statement"]
    assert "examined manifests and reference revision" in statement
    assert "no commit holds" not in statement
    text = (tmp_path / "report" / "a0-results.md").read_text()
    assert "Arm PIT export: `identity-bridge-v4-pit`" in text
    assert "published D0/provenance reference: `identity-bridge-v2-pit`" in text
    (published / "export" / "identity-bridge-v2-pit").rename(published / "export" / "identity-bridge-v8-pit")
    selected = a0.report(base, data_root=data, out=tmp_path / "explicit-report",
                         pit_export="identity-bridge-v4-pit", published_pit_export="identity-bridge-v8-pit")
    assert selected["d0"]["pit"]["d0_pass"]
    assert selected["published_pit_export"] == "identity-bridge-v8-pit"
    assert "identity-bridge-v8-pit" in selected["provenance"]["pit_export"]["manifest"]


def test_symbol_items_reads_only_completed_manifest_parts_nested_or_flat(tmp_path):
    work = tmp_path/'W'
    _evidence(work, 20)
    nested = work/'evidence'/'attempt-new'
    nested.mkdir()
    pq.write_table(pa.table({'value_json':[json.dumps({'items':[{'kind':'symbol_snapshot_match'}]*3})]}),
                   nested/'part-0000.parquet')
    manifest = {'steps':{'reconstruct':{'files':[{'file':'evidence/attempt-new/part-0000.parquet'}]}}}
    (work/'preparation.json').write_text(json.dumps(manifest))
    assert a0.symbol_snapshot_items(work) == 3
    manifest['steps']['reconstruct']['files'] = [{'file':'evidence/part-0000.parquet'}]
    (work/'preparation.json').write_text(json.dumps(manifest))
    assert a0.symbol_snapshot_items(work) == 20
    (work/'preparation.json').write_text(json.dumps({'steps':{}}))
    with pytest.raises(FileNotFoundError, match='completed'):
        a0.symbol_snapshot_items(work)
    manifest['steps']['reconstruct']['files'] = [{'file':'evidence/missing.parquet'}]
    (work/'preparation.json').write_text(json.dumps(manifest))
    with pytest.raises(FileNotFoundError):
        a0.symbol_snapshot_items(work)


def test_batch_option_changes_only_two_native_reconstruction_commands(tmp_path, monkeypatch):
    kwargs = {'data_root':tmp_path/'data','inputs':{'receipt':'r','ops_manifest':'o','document_review':'d'}}
    default = a0.plan_steps(tmp_path/'base','a'*40,**kwargs)
    assert len(default) == 47
    assert default == a0.plan_steps(tmp_path/'base','a'*40,**kwargs,reconstruction_batch_size=None)
    configured = a0.plan_steps(tmp_path/'base','a'*40,**kwargs,reconstruction_batch_size=64)
    changed = []
    for old,new in zip(default,configured,strict=True):
        if old != new:
            changed.append(new['id'])
            assert new == {**old,'argv':old['argv']+['--reconstruction-batch-size','64']}
    assert changed == ['W_A.prepare.reconstruct','W_B2.prepare.reconstruct']
    with pytest.raises(ValueError,match='positive'):
        a0.plan_steps(tmp_path/'base','a'*40,**kwargs,reconstruction_batch_size=0)
    captured = []
    monkeypatch.setattr(a0,'run_arms',lambda *args,**kw:captured.append(kw) or 0)
    assert a0.main(['run-arms','--commit','HEAD','--reconstruction-batch-size','64','--dry-run']) == 0
    assert captured[-1]['reconstruction_batch_size'] == 64
    assert a0.main(['run-arms','--commit','HEAD','--dry-run']) == 0
    assert captured[-1]['reconstruction_batch_size'] is None


def test_existing_arm_execution_batch_binding_cannot_be_retagged(tmp_path):
    base = tmp_path/'base'
    base.mkdir()
    (base/'arms.json').write_text(json.dumps({'commit':'a'*40,'modules':{},'reconstruction_batch_size':64}))
    assert a0.ensure_export(tmp_path,'a'*40,base,{'reconstruction_batch_size':64}) == base/'code'/('a'*7)
    with pytest.raises(SystemExit,match='batch'):
        a0.ensure_export(tmp_path,'a'*40,base,{})
