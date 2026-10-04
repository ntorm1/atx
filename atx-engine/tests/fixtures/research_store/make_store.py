"""Test-only: build a research store from the committed group schema fixtures with Python's SQLite.

Only C++ creates or migrates real stores (atx-engine research/store, sql-design section 3.4). This module exists so
the Python tests (and the C++ gtest that reads a Python-made file) have stores to open without a built executable:
it applies the same policy pragmas as the C++ open (page_size before WAL, synchronous by kind, foreign keys), runs
every ``ddl`` statement of the fixtures (and every view) in one BEGIN IMMEDIATE, stamps ``application_id`` /
``user_version`` and writes ``store_info`` rows ``db_kind`` and ``schema_json``. The ``schema_json`` text is the DB
document of ``atx.store-schema/v1`` assembled from the group objects with ``json.dumps(doc, indent=2) + "\\n"``, which
is byte-identical to what the C++ ``schema_json()`` prints for the same groups (ASCII content).

Usage: python make_store.py OUT.sqlite --db cache --group cache
"""
from __future__ import annotations

import argparse
import json
import sqlite3
from pathlib import Path

SCHEMA_ID = "atx.store-schema/v1"
APPLICATION_IDS = {"catalog": 0x41545843, "cache": 0x4154584B}
SYNCHRONOUS = {"catalog": "FULL", "cache": "NORMAL"}
PAGE_SIZE = 8192
SCHEMA_DIR = Path(__file__).resolve().parent / "schema"


def load_group(name: str) -> dict:
    """One committed group fixture (schema/<name>.json), key order kept."""
    return json.loads((SCHEMA_DIR / f"{name}.json").read_text(encoding="utf-8"))


def document(db_kind: str, groups: list[dict]) -> dict:
    """The DB document {schema, db, application_id, user_version, page_size, groups}."""
    return {"schema": SCHEMA_ID, "db": db_kind, "application_id": APPLICATION_IDS[db_kind],
            "user_version": max(group["version"] for group in groups), "page_size": PAGE_SIZE, "groups": groups}


def schema_text(db_kind: str, groups: list[dict]) -> str:
    return json.dumps(document(db_kind, groups), indent=2) + "\n"


def build(path: Path, db_kind: str, groups: list) -> Path:
    """Create the store at ``path`` (must not exist) holding ``groups`` (group dicts or fixture names)."""
    path = Path(path)
    if path.exists():
        raise FileExistsError(path)
    groups = [load_group(g) if isinstance(g, str) else g for g in groups]
    doc = document(db_kind, groups)
    con = sqlite3.connect(str(path), isolation_level=None)
    try:
        con.execute(f"PRAGMA page_size = {PAGE_SIZE}")
        mode = con.execute("PRAGMA journal_mode = WAL").fetchone()[0]
        if mode != "wal":
            raise RuntimeError(f"journal_mode = WAL refused: {mode}")
        con.execute(f"PRAGMA synchronous = {SYNCHRONOUS[db_kind]}")
        con.execute("PRAGMA foreign_keys = ON")
        con.execute("BEGIN IMMEDIATE")
        for group in groups:
            for table in group["tables"]:
                for statement in table["ddl"]:
                    con.execute(statement)
        for group in groups:
            for view in group["views"]:
                con.execute(view["sql"])
        con.execute(f"PRAGMA application_id = {doc['application_id']}")
        con.execute(f"PRAGMA user_version = {doc['user_version']}")
        con.execute("INSERT INTO store_info(key, value) VALUES (?, ?)", ("db_kind", db_kind))
        con.execute("INSERT INTO store_info(key, value) VALUES (?, ?)",
                    ("schema_json", schema_text(db_kind, groups)))
        con.execute("COMMIT")
    finally:
        con.close()
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", type=Path)
    parser.add_argument("--db", choices=sorted(APPLICATION_IDS), required=True)
    parser.add_argument("--group", action="append", required=True, help="fixture name under schema/ (repeatable)")
    args = parser.parse_args(argv)
    build(args.out, args.db, args.group)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
