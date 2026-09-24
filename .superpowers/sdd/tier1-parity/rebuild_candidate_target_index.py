"""Bounded, same-schema repair of the candidate target ART index only.

Run under run_memory_guarded.py with no other warehouse workload. This script
does not initialize schemas, migrate, restore tables, or resume ingestion.
New repairs require a fresh artifact directory. Explicit completion of an
interrupted repair verifies its original backup and exact catalog state first.
An incomplete/failed repair receipt never authorizes source resumption.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
from typing import Any

import duckdb

TABLE = "identifier_resolution_candidates"
INDEX = "idx_identifier_resolution_candidates_target"
KEY_INDEX = "idx_identifier_resolution_candidates_key"
_EXPECTED_INDEX_COLUMNS = {
    INDEX: "target_security_id",
    KEY_INDEX: "source_dataset_id,source_key_type,source_key_value",
}


def _now() -> str:
    return dt.datetime.now(dt.UTC).isoformat()


def _write_json(path: Path, value: Any) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, indent=2, sort_keys=True, default=str)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def _sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def _literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _connect(path: Path, memory_limit: str, *, read_only: bool) -> duckdb.DuckDBPyConnection:
    con = duckdb.connect(str(path), read_only=read_only, config={
        "memory_limit": memory_limit,
        "threads": "1",
        "preserve_insertion_order": "false",
        "temp_directory": (path.parent / f".{path.name}.duckdb_tmp").as_posix(),
    })
    con.execute("SET TimeZone='UTC'")
    con.execute("PRAGMA disable_progress_bar")
    return con


def _records(con: duckdb.DuckDBPyConnection, sql: str) -> list[dict[str, Any]]:
    result = con.execute(sql)
    columns = [column[0] for column in result.description]
    return [dict(zip(columns, row, strict=True)) for row in result.fetchall()]


def _catalog(con: duckdb.DuckDBPyConnection) -> dict[str, Any]:
    tables = _records(con, f"""SELECT schema_name, table_name, sql
        FROM duckdb_tables() WHERE schema_name='main' AND table_name='{TABLE}'
          AND NOT temporary AND NOT internal""")
    if len(tables) != 1:
        raise RuntimeError("Expected exactly one persistent main candidate table")
    columns = _records(con, f"""SELECT column_name, column_index, data_type,
        is_nullable, column_default FROM duckdb_columns()
        WHERE schema_name='main' AND table_name='{TABLE}' ORDER BY column_index""")
    constraints = _records(con, f"""SELECT constraint_type, constraint_text,
        constraint_column_names FROM duckdb_constraints()
        WHERE schema_name='main' AND table_name='{TABLE}'
        ORDER BY constraint_type, constraint_text""")
    # Include name collisions on other tables/schemas so they fail validation.
    indexes = _records(con, f"""SELECT schema_name, table_name, index_name,
        is_unique, is_primary, expressions, sql FROM duckdb_indexes()
        WHERE (schema_name='main' AND table_name='{TABLE}')
           OR index_name IN ('{INDEX}', '{KEY_INDEX}')
        ORDER BY schema_name, index_name""")
    return {"table": tables[0], "columns": columns, "constraints": constraints, "indexes": indexes}


def _normalized(value: str) -> str:
    return "".join(value.replace('"', "").lower().split()).rstrip(";")


def _validate_catalog(catalog: dict[str, Any]) -> str:
    primary = [row for row in catalog["constraints"] if row["constraint_type"] == "PRIMARY KEY"]
    if len(primary) != 1 or primary[0]["constraint_column_names"] != ["candidate_id"]:
        raise RuntimeError("Refusing repair without the candidate_id primary-key contract")
    required = {"candidate_id", "target_security_id", "source_dataset_id", "source_key_type", "source_key_value"}
    if not required <= {row["column_name"] for row in catalog["columns"]}:
        raise RuntimeError("Candidate table is missing required identity columns")
    indexes = catalog["indexes"]
    if len(indexes) != 2 or {row["index_name"] for row in indexes} != set(_EXPECTED_INDEX_COLUMNS):
        raise RuntimeError("Refusing unexpected candidate index catalog")
    target_sql = ""
    for row in indexes:
        columns = _EXPECTED_INDEX_COLUMNS[row["index_name"]]
        expected_sql = f"CREATE INDEX {row['index_name']} ON {TABLE}({columns})"
        if (row["schema_name"] != "main" or row["table_name"] != TABLE
                or row["is_unique"] or row["is_primary"]
                or _normalized(row["expressions"]).strip("[]") != columns
                or _normalized(row["sql"]) != _normalized(expected_sql)):
            raise RuntimeError("Refusing nonoptional, misowned, or unexpected index definition")
        if row["index_name"] == INDEX:
            target_sql = row["sql"]
    return target_sql


@contextlib.contextmanager
def _sequential(con: duckdb.DuckDBPyConnection):
    count, percentage = con.execute("""SELECT current_setting('index_scan_max_count'),
        current_setting('index_scan_percentage')""").fetchone()
    con.execute("SET index_scan_max_count=0")
    con.execute("SET index_scan_percentage=0")
    try:
        yield
    finally:
        con.execute("SET index_scan_max_count=?", [count])
        con.execute("SET index_scan_percentage=?", [percentage])


def _row_evidence(con: duckdb.DuckDBPyConnection, relation: str) -> dict[str, Any]:
    digest = hashlib.sha256()
    count = 0
    with _sequential(con):
        result = con.execute(f"SELECT * FROM {relation} ORDER BY candidate_id")
        while rows := result.fetchmany(256):
            for row in rows:
                encoded = json.dumps(row, ensure_ascii=False, separators=(",", ":"), default=str).encode("utf-8")
                digest.update(len(encoded).to_bytes(8, "big"))
                digest.update(encoded)
                count += 1
    return {"rows": count, "ordered_row_sha256": digest.hexdigest(),
            "encoding": "utf8-json-array-default-str-length-prefix-u64be-v1"}


def _lookup(con: duckdb.DuckDBPyConnection, candidate_id: str, target: str) -> dict[str, Any]:
    # ORDER BY plus LIMIT induces a TopN sequential plan on tiny tables. Keep
    # the SQL bound and sort only the bounded result in Python so the actual
    # default index scan remains part of acceptance, including small fixtures.
    sql = f"SELECT candidate_id FROM main.{TABLE} WHERE target_security_id=? LIMIT 1001"
    default_rows = sorted(con.execute(sql, [target]).fetchall())
    plan = "\n".join(str(row[-1]) for row in con.execute("EXPLAIN ANALYZE " + sql, [target]).fetchall())
    with _sequential(con):
        sequential_rows = sorted(con.execute(sql, [target]).fetchall())
    if len(default_rows) > 1000 or len(sequential_rows) > 1000:
        raise RuntimeError("Acceptance target exceeds the bounded 1000-candidate lookup")
    if (candidate_id,) not in sequential_rows:
        raise RuntimeError("Acceptance candidate does not exist at the expected target in table rows")
    return {"default_candidate_ids": [row[0] for row in default_rows],
            "sequential_candidate_ids": [row[0] for row in sequential_rows],
            "default_uses_index": "Index Scan" in plan,
            "equivalent": default_rows == sequential_rows}


def _backup_equality(con: duckdb.DuckDBPyConnection, backup: Path) -> None:
    relation = f"read_parquet({_literal(backup.as_posix())})"
    with _sequential(con):
        for left, right in ((f"main.{TABLE}", relation), (relation, f"main.{TABLE}")):
            mismatch = con.execute(f"""SELECT EXISTS(
                SELECT * FROM {left} EXCEPT ALL SELECT * FROM {right})""").fetchone()[0]
            if mismatch:
                raise RuntimeError("Candidate backup differs from live table rows")


def _create_target_index(con: duckdb.DuckDBPyConnection, create_sql: str) -> None:
    # Do not combine DROP and CREATE in one transaction: installed DuckDB
    # rejects same-name ART replacement at COMMIT with CreateDeltaIndex.
    con.execute(create_sql)


def _recovery_state(con: duckdb.DuckDBPyConnection, before: dict[str, Any],
                    rows: dict[str, Any], backup: Path) -> str:
    """Accept only unchanged rows with the original index present or absent."""
    current = _catalog(con)
    absent = {**before, "indexes": [row for row in before["indexes"] if row["index_name"] != INDEX]}
    if current == before:
        index_state = "present"
    elif current == absent:
        index_state = "absent"
    else:
        raise RuntimeError("Refusing recovery after unexpected candidate catalog drift")
    if _row_evidence(con, f"main.{TABLE}") != rows:
        raise RuntimeError("Refusing recovery after candidate row count or digest changed")
    if _row_evidence(con, f"read_parquet({_literal(backup.as_posix())})") != rows:
        raise RuntimeError("Refusing recovery from a backup with a changed row count or digest")
    _backup_equality(con, backup)
    return index_state


def _verify(con: duckdb.DuckDBPyConnection, before: dict[str, Any], rows: dict[str, Any],
            backup: Path, candidate_id: str, target: str) -> dict[str, Any]:
    after = _catalog(con)
    _validate_catalog(after)
    if after != before:
        raise RuntimeError("Candidate columns, constraints, table, or index definitions changed")
    after_rows = _row_evidence(con, f"main.{TABLE}")
    if after_rows != rows:
        raise RuntimeError("Candidate row count or logical digest changed")
    _backup_equality(con, backup)
    lookup = _lookup(con, candidate_id, target)
    if not lookup["equivalent"] or not lookup["default_uses_index"]:
        raise RuntimeError("Rebuilt target index did not pass the indexed/sequential acceptance query")
    return {"catalog": after, "rows": after_rows, "lookup": lookup}


def _resume_inputs(artifact_dir: Path, db_path: Path, candidate_id: str,
                   target_security_id: str, memory_limit: str) -> tuple[dict[str, Any], dict[str, Any], Path]:
    state = json.loads((artifact_dir / "repair-state.json").read_text(encoding="utf-8"))
    allowed_phases = {"drop_started", "index_absent", "index_absent_durable", "create_started", "index_recreated"}
    if (state.get("status") not in ("running", "failed") or state.get("phase") not in allowed_phases
            or state.get("inspect_only") is not False or state.get("safe_to_resume_source") is not False
            or state.get("repair_required") is not True
            or state.get("db_path") != str(db_path) or state.get("artifact_dir") != str(artifact_dir)
            or state.get("candidate_id") != candidate_id or state.get("target_security_id") != target_security_id
            or state.get("memory_limit") != memory_limit or state.get("threads") != 1):
        raise RuntimeError("Refusing incompatible or non-resumable repair state")
    manifest = artifact_dir / "catalog-before.json"
    backup = artifact_dir / f"{TABLE}.parquet"
    if (state.get("backup_path") != str(backup)
            or state.get("backup_bytes") != backup.stat().st_size
            or state.get("backup_sha256") != _sha256(backup)
            or state.get("before_manifest_sha256") != _sha256(manifest)):
        raise RuntimeError("Refusing recovery after retained backup or manifest hash changed")
    before_manifest = json.loads(manifest.read_text(encoding="utf-8"))
    _validate_catalog(before_manifest["catalog"])
    if state.get("before_rows") != before_manifest["rows"] or state.get("backup_rows") != before_manifest["rows"]:
        raise RuntimeError("Refusing inconsistent saved candidate row evidence")
    return state, before_manifest, backup


def run_repair(*, db_path: Path, candidate_id: str, target_security_id: str,
               artifact_dir: Path | None = None, resume_artifact_dir: Path | None = None,
               memory_limit: str = "256MB", inspect_only: bool = False) -> dict[str, Any]:
    if memory_limit not in ("256MB", "512MB"):
        raise ValueError("Only the bounded 256MB and 512MB profiles are supported")
    db_path = db_path.resolve(strict=True)
    if not db_path.is_file():
        raise ValueError("Warehouse must be an existing regular file")
    if (artifact_dir is None) == (resume_artifact_dir is None):
        raise ValueError("Supply exactly one new artifact directory or an explicit resume directory")
    if resume_artifact_dir is not None and inspect_only:
        raise ValueError("Inspection cannot resume a repair")
    resuming = resume_artifact_dir is not None
    before_manifest = None
    if resuming:
        artifact_dir = resume_artifact_dir.resolve(strict=True)
        attempt = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%S%fZ")
        try:
            state, before_manifest, backup = _resume_inputs(
                artifact_dir, db_path, candidate_id, target_security_id, memory_limit,
            )
        except BaseException as exc:
            _write_json(artifact_dir / f"resume-refused-{attempt}.json", {
                "status": "failed", "phase": "resume_artifact_preflight", "finished_at": _now(),
                "error_type": type(exc).__name__, "error": str(exc), "safe_to_resume_source": False,
            })
            raise
        history_name = f"state-before-resume-{attempt}.json"
        _write_json(artifact_dir / history_name, state)
        for field in ("error_type", "error", "failed_phase", "finished_at"):
            state.pop(field, None)
        state.update(status="running", resumed_at=_now(), resumed_from_state=history_name)
    else:
        artifact_dir = artifact_dir.resolve()
        artifact_dir.mkdir(parents=True, exist_ok=False)
        state = {"status": "running", "phase": "preflight", "started_at": _now(),
                 "db_path": str(db_path), "artifact_dir": str(artifact_dir),
                 "candidate_id": candidate_id, "target_security_id": target_security_id,
                 "memory_limit": memory_limit, "threads": 1, "inspect_only": inspect_only,
                 "repair_required": True, "safe_to_resume_source": False}
    state_path = artifact_dir / "repair-state.json"
    _write_json(state_path, state)
    con = None
    try:
        con = _connect(db_path, memory_limit, read_only=inspect_only)
        manifest_path = artifact_dir / "catalog-before.json"
        if resuming:
            before = before_manifest["catalog"]
            before_rows = before_manifest["rows"]
            create_sql = _validate_catalog(before)
            index_state = _recovery_state(con, before, before_rows, backup)
            state["resume_index_state"] = index_state
        else:
            before = _catalog(con)
            create_sql = _validate_catalog(before)
            before_rows = _row_evidence(con, f"main.{TABLE}")
            lookup = _lookup(con, candidate_id, target_security_id)
            before_manifest = {"catalog": before, "rows": before_rows, "lookup": lookup,
                               "duckdb_version": duckdb.__version__, "recorded_at": _now()}
            _write_json(manifest_path, before_manifest)
            state.update(before_rows=before_rows, before_lookup=lookup,
                         before_manifest_sha256=_sha256(manifest_path))
        if inspect_only:
            state.update(status="inspected", phase="inspection_complete", finished_at=_now())
            _write_json(state_path, state)
            _write_json(artifact_dir / "receipt.json", state)
            return state

        if not resuming:
            con.execute("CHECKPOINT")
            backup = artifact_dir / f"{TABLE}.parquet"
            con.execute(f"COPY (SELECT * FROM main.{TABLE} ORDER BY candidate_id) "
                        f"TO {_literal(backup.as_posix())} (FORMAT PARQUET, COMPRESSION ZSTD)")
            with backup.open("r+b") as handle:
                handle.flush()
                os.fsync(handle.fileno())
            backup_rows = _row_evidence(con, f"read_parquet({_literal(backup.as_posix())})")
            if backup_rows != before_rows:
                raise RuntimeError("Candidate Parquet backup failed count/digest verification")
            _backup_equality(con, backup)
            state.update(phase="backup_verified", backup_path=str(backup), backup_bytes=backup.stat().st_size,
                         backup_sha256=_sha256(backup), backup_rows=backup_rows)
            _write_json(state_path, state)
            state["phase"] = "drop_started"
            _write_json(state_path, state)
            con.execute(f"DROP INDEX main.{INDEX}")
            state["phase"] = "index_absent"
            _write_json(state_path, state)
            con.execute("CHECKPOINT")
            con.close()
            con = None
            con = _connect(db_path, memory_limit, read_only=False)
            index_state = _recovery_state(con, before, before_rows, backup)
            if index_state != "absent":
                raise RuntimeError("Expected only the target index to be absent after committed DROP")
            state["phase"] = "index_absent_durable"
            _write_json(state_path, state)
        if index_state == "absent":
            state["phase"] = "create_started"
            _write_json(state_path, state)
            _create_target_index(con, create_sql)
        state["phase"] = "index_recreated"
        _write_json(state_path, state)
        _verify(con, before, before_rows, backup, candidate_id, target_security_id)
        con.execute("CHECKPOINT")
        con.close()
        con = None
        con = _connect(db_path, memory_limit, read_only=True)
        final = _verify(con, before, before_rows, backup, candidate_id, target_security_id)
        if _sha256(backup) != state["backup_sha256"] or _sha256(manifest_path) != state["before_manifest_sha256"]:
            raise RuntimeError("A retained backup artifact changed during repair")
        after_path = artifact_dir / "catalog-after.json"
        _write_json(after_path, final)
        state.update(status="completed", phase="durable_verified", repair_required=False,
                     safe_to_resume_source=True, finished_at=_now(), after_rows=final["rows"],
                     after_lookup=final["lookup"], after_manifest_sha256=_sha256(after_path))
        _write_json(state_path, state)
        _write_json(artifact_dir / "receipt.json", state)
        return state
    except BaseException as exc:
        state.update(status="failed", failed_phase=state["phase"], error_type=type(exc).__name__,
                     error=str(exc), finished_at=_now(), repair_required=True, safe_to_resume_source=False)
        _write_json(state_path, state)
        _write_json(artifact_dir / "receipt.json", state)
        raise
    finally:
        if con is not None:
            con.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", required=True, type=Path)
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--artifact-dir", type=Path)
    output.add_argument("--resume-artifact-dir", type=Path,
                        help="Complete an interrupted repair only after original backup/catalog verification")
    parser.add_argument("--candidate-id", required=True)
    parser.add_argument("--target-security-id", required=True)
    parser.add_argument("--memory-limit", choices=("256MB", "512MB"), default="256MB")
    parser.add_argument("--inspect-only", action="store_true", help="Read-only inspection; never rebuild or authorize ingestion")
    args = parser.parse_args()
    try:
        result = run_repair(**vars(args))
    except Exception as exc:
        print(json.dumps({"status": "failed", "error_type": type(exc).__name__, "error": str(exc),
                          "artifact_dir": str(args.artifact_dir or args.resume_artifact_dir),
                          "safe_to_resume_source": False}))
        return 2
    print(json.dumps({key: result[key] for key in ("status", "phase", "artifact_dir", "safe_to_resume_source")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
