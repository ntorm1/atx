"""Operational index repair preserves candidate identity, clocks, and provenance."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path

import duckdb
import pytest

_SCRIPT = Path(__file__).resolve().parents[2] / ".superpowers/sdd/tier1-parity/rebuild_candidate_target_index.py"
_SPEC = importlib.util.spec_from_file_location("candidate_target_index_repair", _SCRIPT)
assert _SPEC is not None and _SPEC.loader is not None
repair = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(repair)


def _database(path: Path, *, collision: str | None = None) -> Path:
    with duckdb.connect(str(path), config={"memory_limit": "64MB", "threads": 1}) as con:
        primary = "PRIMARY KEY" if collision != "no_primary_key" else "NOT NULL"
        con.execute(f"""CREATE TABLE identifier_resolution_candidates (
            candidate_id VARCHAR {primary}, source_dataset_id VARCHAR NOT NULL,
            source_key_type VARCHAR NOT NULL, source_key_value VARCHAR NOT NULL,
            target_security_id VARCHAR NOT NULL, candidate_status VARCHAR NOT NULL,
            confidence DOUBLE NOT NULL, available_at TIMESTAMP, details_json VARCHAR,
            run_id VARCHAR, source_loaded_at TIMESTAMP NOT NULL DEFAULT now())""")
        con.execute("""INSERT INTO identifier_resolution_candidates VALUES
            ('candidate-a', 'sec_company_facts', 'CIK', '0001495229', 'issuer-a',
             'proposed', 0, TIMESTAMP '2026-01-15 22:00:00', '{"unresolved":1}',
             'original-source-run', TIMESTAMP '2026-01-16 02:03:04.567890'),
            ('candidate-b', 'figi', 'CUSIP', '123456789', 'issuer-b',
             'rejected', 0.25, TIMESTAMP '2026-03-15 22:00:00', NULL,
             'other-source-run', TIMESTAMP '2026-03-16 02:03:04.567890'),
            ('candidate-c', 'sec_company_facts', 'CIK', '0000000003', 'issuer-c',
             'proposed', 0, NULL, '{"unresolved":2}', NULL,
             TIMESTAMP '2026-06-16 02:03:04.567890')""")
        con.execute("""CREATE INDEX idx_identifier_resolution_candidates_key
            ON identifier_resolution_candidates(source_dataset_id, source_key_type, source_key_value)""")
        con.execute("CREATE TABLE unrelated_control (id INTEGER PRIMARY KEY, target_security_id VARCHAR NOT NULL)")
        con.execute("INSERT INTO unrelated_control VALUES (1, 'untouched')")
        owner = "unrelated_control" if collision == "wrong_owner" else "identifier_resolution_candidates"
        column = "source_key_value" if collision == "wrong_column" else "target_security_id"
        unique = "UNIQUE" if collision == "unique" else ""
        con.execute(f"CREATE {unique} INDEX idx_identifier_resolution_candidates_target ON {owner}({column})")
        con.execute("CHECKPOINT")
    return path


def _rows(path: Path):
    with duckdb.connect(str(path), read_only=True, config={"memory_limit": "64MB", "threads": 1}) as con:
        return con.execute("SELECT * FROM identifier_resolution_candidates ORDER BY candidate_id").fetchall()


def _call(path: Path, artifacts: Path, **kwargs):
    return repair.run_repair(db_path=path, artifact_dir=artifacts,
                             candidate_id="candidate-a", target_security_id="issuer-a", **kwargs)


def test_two_phase_rebuild_preserves_rows_constraints_and_candidate_reentry(tmp_path):
    path = _database(tmp_path / "candidates.duckdb")
    expected = _rows(path)
    artifacts = tmp_path / "repair1"
    result = _call(path, artifacts)
    assert result["status"] == "completed"
    assert result["safe_to_resume_source"] and not result["repair_required"]
    assert result["before_rows"] == result["after_rows"] == result["backup_rows"]
    assert result["after_rows"]["rows"] == 3
    assert result["after_lookup"]["default_uses_index"]
    assert result["after_lookup"]["default_candidate_ids"] == ["candidate-a"]
    assert result["after_lookup"]["equivalent"]
    backup = Path(result["backup_path"])
    assert hashlib.sha256(backup.read_bytes()).hexdigest() == result["backup_sha256"]
    assert _rows(path) == expected
    with duckdb.connect(str(path), config={"memory_limit": "64MB", "threads": 1}) as con:
        assert con.execute("SELECT * FROM unrelated_control").fetchall() == [(1, "untouched")]
        # Exercise the real source's DELETE plus INSERT of the same candidate ID
        # through COMMIT, then checkpoint/reopen. No provenance value is refreshed.
        con.execute("BEGIN TRANSACTION")
        con.execute("DELETE FROM identifier_resolution_candidates WHERE candidate_id='candidate-a'")
        con.execute("INSERT INTO identifier_resolution_candidates SELECT * FROM read_parquet(?) "
                    "WHERE candidate_id='candidate-a'", [str(backup)])
        con.execute("COMMIT")
        assert con.execute("SELECT candidate_id FROM identifier_resolution_candidates "
                           "WHERE target_security_id='issuer-a'").fetchall() == [("candidate-a",)]
        assert con.execute("SELECT candidate_id FROM identifier_resolution_candidates "
                           "WHERE available_at <= TIMESTAMP '2026-02-01' ORDER BY candidate_id").fetchall() == [
                               ("candidate-a",),
                           ]
        with pytest.raises(duckdb.ConstraintException):
            con.execute("INSERT INTO identifier_resolution_candidates SELECT * FROM read_parquet(?) "
                        "WHERE candidate_id='candidate-a'", [str(backup)])
        with pytest.raises(duckdb.ConstraintException):
            con.execute("UPDATE identifier_resolution_candidates SET candidate_status=NULL "
                        "WHERE candidate_id='candidate-a'")
        con.execute("CHECKPOINT")
    assert _rows(path) == expected


def test_inspection_and_existing_artifact_refusal_do_not_mutate_database(tmp_path):
    path = _database(tmp_path / "inspect.duckdb")
    before = hashlib.sha256(path.read_bytes()).hexdigest()
    artifacts = tmp_path / "inspection"
    result = _call(path, artifacts, inspect_only=True)
    assert result["status"] == "inspected" and not result["safe_to_resume_source"]
    assert not (artifacts / "identifier_resolution_candidates.parquet").exists()
    assert hashlib.sha256(path.read_bytes()).hexdigest() == before
    with pytest.raises(FileExistsError):
        _call(path, artifacts)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == before


@pytest.mark.parametrize("collision", ["unique", "wrong_owner", "wrong_column", "no_primary_key"])
def test_unexpected_index_or_identity_contract_is_refused_before_backup_or_drop(tmp_path, collision):
    path = _database(tmp_path / f"{collision}.duckdb", collision=collision)
    expected = _rows(path)
    with duckdb.connect(str(path), read_only=True, config={"memory_limit": "64MB", "threads": 1}) as con:
        before = repair._catalog(con)
    artifacts = tmp_path / "refusal"
    with pytest.raises(RuntimeError, match="Refusing"):
        _call(path, artifacts)
    assert _rows(path) == expected
    with duckdb.connect(str(path), read_only=True, config={"memory_limit": "64MB", "threads": 1}) as con:
        assert repair._catalog(con) == before
    receipt = json.loads((artifacts / "receipt.json").read_text())
    assert receipt["status"] == "failed" and receipt["failed_phase"] == "preflight"
    assert receipt["repair_required"] and not receipt["safe_to_resume_source"]
    assert not (artifacts / "identifier_resolution_candidates.parquet").exists()


def _leave_index_absent(path, artifacts, monkeypatch):
    original = repair._create_target_index

    def fail_recreation(con, create_sql):
        original(con, create_sql.replace("target_security_id", "nonexistent_column"))

    with monkeypatch.context() as patch:
        patch.setattr(repair, "_create_target_index", fail_recreation)
        with pytest.raises(duckdb.BinderException):
            _call(path, artifacts)


def _resume(path, artifacts):
    return repair.run_repair(db_path=path, resume_artifact_dir=artifacts,
                             candidate_id="candidate-a", target_security_id="issuer-a")


def test_failed_recreation_retains_backup_and_only_absent_target_then_completes_verified_resume(tmp_path, monkeypatch):
    path = _database(tmp_path / "resume.duckdb")
    expected = _rows(path)
    artifacts = tmp_path / "failed-repair"
    with duckdb.connect(str(path), read_only=True, config={"memory_limit": "64MB", "threads": 1}) as con:
        before = repair._catalog(con)
    _leave_index_absent(path, artifacts, monkeypatch)
    receipt = json.loads((artifacts / "receipt.json").read_text())
    assert receipt["failed_phase"] == "create_started"
    assert receipt["repair_required"] and not receipt["safe_to_resume_source"]
    assert Path(receipt["backup_path"]).is_file()
    assert _rows(path) == expected
    with duckdb.connect(str(path), read_only=True, config={"memory_limit": "64MB", "threads": 1}) as con:
        current = repair._catalog(con)
        assert current == {**before, "indexes": [row for row in before["indexes"]
                                                if row["index_name"] != repair.INDEX]}
    result = _resume(path, artifacts)
    assert result["status"] == "completed" and result["safe_to_resume_source"]
    assert not {"error_type", "error", "failed_phase"} & result.keys()
    assert result["resume_index_state"] == "absent"
    assert result["backup_sha256"] == receipt["backup_sha256"]
    assert list(artifacts.glob("state-before-resume-*.json"))
    assert _rows(path) == expected
    with duckdb.connect(str(path), read_only=True, config={"memory_limit": "64MB", "threads": 1}) as con:
        assert repair._catalog(con) == before
        assert con.execute("SELECT candidate_id FROM identifier_resolution_candidates "
                           "WHERE target_security_id='issuer-a'").fetchall() == [("candidate-a",)]


@pytest.mark.parametrize("change", ["backup", "manifest", "rows", "catalog"])
def test_resume_refuses_changed_backup_rows_or_catalog_without_creating_index(tmp_path, monkeypatch, change):
    path = _database(tmp_path / f"refused-resume-{change}.duckdb")
    artifacts = tmp_path / "incomplete-repair"
    _leave_index_absent(path, artifacts, monkeypatch)
    if change == "backup":
        with (artifacts / "identifier_resolution_candidates.parquet").open("ab") as handle:
            handle.write(b"tampered")
    elif change == "manifest":
        with (artifacts / "catalog-before.json").open("a") as handle:
            handle.write(" ")
    else:
        with duckdb.connect(str(path), config={"memory_limit": "64MB", "threads": 1}) as con:
            if change == "rows":
                con.execute("UPDATE identifier_resolution_candidates SET run_id='unexpected-new-writer' "
                            "WHERE candidate_id='candidate-b'")
            else:
                con.execute("CREATE INDEX unexpected_candidate_index ON identifier_resolution_candidates(confidence)")
    expected = _rows(path)
    with pytest.raises(RuntimeError, match="Refusing recovery"):
        _resume(path, artifacts)
    assert _rows(path) == expected
    with duckdb.connect(str(path), read_only=True, config={"memory_limit": "64MB", "threads": 1}) as con:
        assert con.execute("SELECT count(*) FROM duckdb_indexes() "
                           "WHERE index_name='idx_identifier_resolution_candidates_target'").fetchone() == (0,)
    state = json.loads((artifacts / "repair-state.json").read_text())
    assert state["repair_required"] and not state["safe_to_resume_source"]


def test_resume_after_create_committed_only_verifies_existing_index(tmp_path, monkeypatch):
    path = _database(tmp_path / "interrupted-verification.duckdb")
    expected = _rows(path)
    artifacts = tmp_path / "created-index"

    def interrupted_verification(*args, **kwargs):
        raise RuntimeError("interrupted after CREATE committed")

    with monkeypatch.context() as patch:
        patch.setattr(repair, "_verify", interrupted_verification)
        with pytest.raises(RuntimeError, match="interrupted after CREATE committed"):
            _call(path, artifacts)
    state = json.loads((artifacts / "repair-state.json").read_text())
    assert state["phase"] == "index_recreated" and not state["safe_to_resume_source"]
    result = _resume(path, artifacts)
    assert result["status"] == "completed" and result["resume_index_state"] == "present"
    assert result["after_lookup"]["default_uses_index"]
    assert result["after_lookup"]["equivalent"]
    assert _rows(path) == expected


def test_backup_equality_rejects_a_lost_candidate(tmp_path):
    path = _database(tmp_path / "backup-integrity.duckdb")
    incomplete = tmp_path / "incomplete.parquet"
    with duckdb.connect(str(path), config={"memory_limit": "64MB", "threads": 1}) as con:
        con.execute("COPY (SELECT * FROM identifier_resolution_candidates "
                    "WHERE candidate_id<>'candidate-b') TO ? (FORMAT PARQUET)", [str(incomplete)])
        with pytest.raises(RuntimeError, match="backup differs"):
            repair._backup_equality(con, incomplete)
