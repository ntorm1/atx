"""Test-only: (re)build ``py_created.sqlite``, a catalog store made by Python's SQLite (3.43.1 on the host).

The gtest ``ResearchStoreOpen.ReadsPythonCreatedFixture`` opens a copy of it with the C++ store (vendored SQLite
3.53.2) and reads these rows back: the cross-version file-compatibility check of sql-design section 3.6 (drift guard
6). ``test_research_store_fixtures.py`` rebuilds it into a temporary directory and compares ``logical_content`` with
the committed file, so the binary fixture can only change together with this script.

Usage: python make_py_fixture.py   (rewrites py_created.sqlite beside this file)
"""
from __future__ import annotations

import sqlite3
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_store  # noqa: E402

FIXTURE = HERE / "py_created.sqlite"
A, B, C, D, E = ("a" * 64, "b" * 64, "c" * 64, "d" * 64, "e" * 64)

# Row values; the gtest literals repeat them (atx-engine/tests/research/research_store_open_test.cpp).
ROWS = {
    "artifact": [
        ("scripts/specs/p9/a.json", "scripts/specs/p9/A.json", A, 120, "spec", "atx.spec/v1", "verified", None,
         "lf", None),
        ("build-equity/runs/r1/payload.f64", "build-equity/runs/r1/payload.f64", B, None, "payload", None,
         "declared", "build-equity/runs/r1/manifest.json", None, C),
    ],
    "producer": [(C, "engine", D, "0123abc", "Debug", None, None, E)],
    "catalog_run": [("run-1", "0123abc", None, '["scripts/specs"]', "2023-01-02T00:00:00Z", 1.5, 3, 1, 1, 1, None)],
    "artifact_seen": [("scripts/specs/p9/a.json", A, "run-1")],
    "skipped_path": [("run-1", "build-equity/other", "outside-roots")],
}
COLUMNS = {
    "artifact": "path_key, path, sha256, bytes, class, json_schema, sha_source, declared_by, eol, producer_key",
    "producer": "producer_key, kind, exe_sha256, git_sha, build_type, module, code_sha256, receipt_sha256",
    "catalog_run": "catalog_run_id, git_sha, store_exe_sha256, roots, started_utc, seconds, files_seen, "
                   "files_verified, files_declared, files_skipped, catalog_digest",
    "artifact_seen": "path_key, sha256, catalog_run_id",
    "skipped_path": "catalog_run_id, path, reason",
}


def build(path: Path) -> Path:
    make_store.build(path, "catalog", ["catalog_core"])
    con = sqlite3.connect(str(path), isolation_level=None)
    try:
        con.execute("PRAGMA foreign_keys = ON")
        con.execute("BEGIN IMMEDIATE")
        for table, rows in ROWS.items():
            marks = ", ".join("?" * len(rows[0]))
            con.executemany(f"INSERT INTO {table}({COLUMNS[table]}) VALUES ({marks})", rows)
        con.execute("COMMIT")
        con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    finally:
        con.close()
    return path


def logical_content(path: Path) -> dict:
    """Everything a reader can observe except page layout: pragmas, schema objects, every row in key order."""
    con = sqlite3.connect(f"{Path(path).resolve().as_uri()}?mode=ro", uri=True)
    try:
        pragmas = {name: con.execute(f"PRAGMA {name}").fetchone()[0]
                   for name in ("application_id", "user_version", "page_size", "journal_mode")}
        schema = con.execute("SELECT type, name, tbl_name, sql FROM sqlite_schema ORDER BY type, name").fetchall()
        tables = [name for kind, name, _, _ in schema if kind == "table"]
        rows = {name: con.execute(f"SELECT * FROM {name} ORDER BY 1, 2").fetchall() for name in tables}
    finally:
        con.close()
    return {"pragmas": pragmas, "schema": schema, "rows": rows}


def main() -> int:
    for suffix in ("", "-wal", "-shm"):
        Path(str(FIXTURE) + suffix).unlink(missing_ok=True)
    build(FIXTURE)
    print(FIXTURE, FIXTURE.stat().st_size)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
