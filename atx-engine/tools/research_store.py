"""Generic Python accessor of a research store (``atx.store-schema/v1``), driven by the schema the store carries.

The store's tables are compile-time C++ descriptors (atx-engine ``research/store``, ruling SQL-5); C++ alone creates
and migrates a store and stores the schema document it printed in ``store_info('schema_json')`` (ruling SQL-10).
This module reads that row and nothing else to learn the tables: no table or column name is written here except the
bootstrap row itself. It never runs DDL, never migrates, and computes no digest.

``Store.open(path)`` opens an existing file (never creates one) with ``foreign_keys=ON``, ``trusted_schema=OFF``,
``busy_timeout=30000`` and ``isolation_level=None`` (autocommit; ``transaction()`` is an explicit
``BEGIN IMMEDIATE``), and refuses (``StoreError``) a file without the schema row, a schema document other than
``atx.store-schema/v1``, or a ``user_version`` / ``application_id`` different from the document's.

Generic per table: ``insert(table, row)``, ``upsert(table, row)``, ``get(table, key)``, ``select(table, where)``
(always ``ORDER BY`` the primary key). Python values are checked against the column type before SQLite sees them:
``int`` for int (64-bit) and u64 (0 .. 2**64-1, stored as its signed 64-bit pattern), ``bool`` for bool, ``float``
for real (NaN and -0.0 refused: SQLite would store NULL / +0.0), ``str`` for text / sha256 / relpath / json,
``bytes`` for blob, ``None`` only for a nullable column. Value CHECKs (sha256 form, relative paths, JSON validity,
allowed values, append-only triggers) are SQLite's and surface as ``sqlite3.IntegrityError``.

One ``Store`` per thread (Python's sqlite3 connections are bound to their thread).
"""
from __future__ import annotations

import contextlib
import json
import math
import re
import sqlite3
from pathlib import Path

SCHEMA_ID = "atx.store-schema/v1"
TEXT_TYPES = frozenset({"text", "sha256", "relpath", "json"})
TYPES = frozenset({"int", "bool", "real", "u64", "blob"}) | TEXT_TYPES
_IDENTIFIER = re.compile(r"[a-z_][a-z0-9_]{0,63}")
_I64_MIN, _I64_MAX = -(1 << 63), (1 << 63) - 1
_U64_MAX = (1 << 64) - 1


class StoreError(ValueError):
    """A store this module refuses to open, or a value it refuses to write."""


def _identifier(name) -> str:
    if not isinstance(name, str) or not _IDENTIFIER.fullmatch(name):
        raise StoreError(f"schema names a non-identifier {name!r}")
    return name


class Column:
    __slots__ = ("name", "type", "nullable")

    def __init__(self, doc: dict):
        self.name = _identifier(doc.get("name"))
        self.type = doc.get("type")
        if self.type not in TYPES:
            raise StoreError(f"column {self.name}: unknown type {self.type!r}")
        self.nullable = doc.get("nullable") is True


class Table:
    """One table of the schema document: columns in declared order and the primary key."""

    def __init__(self, doc: dict):
        self.name = _identifier(doc.get("name"))
        self.columns = [Column(c) for c in doc.get("columns", [])]
        self.by_name = {c.name: c for c in self.columns}
        self.key = [_identifier(k) for k in doc.get("key", [])]
        if not self.columns or not self.key or any(k not in self.by_name for k in self.key):
            raise StoreError(f"table {self.name}: no columns or an unknown key column")
        names = ", ".join(c.name for c in self.columns)
        self.insert_sql = f"INSERT INTO {self.name}({names}) VALUES ({', '.join('?' * len(self.columns))})"
        updates = ", ".join(f"{c.name} = excluded.{c.name}" for c in self.columns if c.name not in self.key)
        conflict = f" ON CONFLICT({', '.join(self.key)}) DO " + (f"UPDATE SET {updates}" if updates else "NOTHING")
        self.upsert_sql = self.insert_sql + conflict
        self.select_sql = f"SELECT {names} FROM {self.name}"
        self.order_sql = f" ORDER BY {', '.join(self.key)}"

    def to_sql(self, column: Column, value):
        where = f"{self.name}.{column.name}"
        if value is None:
            if not column.nullable:
                raise StoreError(f"{where}: None in a NOT NULL column")
            return None
        kind = column.type
        if kind == "int":
            if type(value) is not int or not _I64_MIN <= value <= _I64_MAX:
                raise StoreError(f"{where}: int expects a 64-bit int, got {value!r}")
            return value
        if kind == "u64":
            if type(value) is not int or not 0 <= value <= _U64_MAX:
                raise StoreError(f"{where}: u64 expects an int in [0, 2**64), got {value!r}")
            return value - (1 << 64) if value > _I64_MAX else value
        if kind == "bool":
            if type(value) is not bool:
                raise StoreError(f"{where}: bool expects a bool, got {value!r}")
            return 1 if value else 0
        if kind == "real":
            if type(value) is not float:
                raise StoreError(f"{where}: real expects a float, got {value!r}")
            if math.isnan(value) or (value == 0.0 and math.copysign(1.0, value) < 0):
                raise StoreError(f"{where}: a REAL column cannot hold NaN or -0.0 losslessly")
            return value
        if kind == "blob":
            if type(value) is not bytes:
                raise StoreError(f"{where}: blob expects bytes, got {type(value).__name__}")
            return value
        if type(value) is not str:
            raise StoreError(f"{where}: {kind} expects a str, got {type(value).__name__}")
        return value

    @staticmethod
    def from_sql(column: Column, value):
        if value is None:
            return None
        if column.type == "u64":
            return value & _U64_MAX
        if column.type == "bool":
            return bool(value)
        if column.type == "real":
            return float(value)
        if column.type == "blob":
            return bytes(value)
        return value

    def row_values(self, row: dict) -> list:
        if not isinstance(row, dict):
            raise StoreError(f"{self.name}: a row is a dict")
        unknown = [k for k in row if k not in self.by_name]
        if unknown:
            raise StoreError(f"{self.name}: unknown columns {unknown}")
        return [self.to_sql(c, row.get(c.name)) for c in self.columns]

    def key_values(self, key: dict) -> list:
        if not isinstance(key, dict) or sorted(key) != sorted(self.key):
            raise StoreError(f"{self.name}: a key is a dict of exactly {self.key}")
        return [self.to_sql(self.by_name[k], key[k]) for k in self.key]

    def to_row(self, values) -> dict:
        return {c.name: self.from_sql(c, v) for c, v in zip(self.columns, values)}


class Store:
    """An open research store (see the module docstring)."""

    def __init__(self, connection: sqlite3.Connection, path: Path, schema_text: str, document: dict):
        self._con = connection
        self.path = path
        self.schema_text = schema_text
        self.schema = document
        self._tables: dict[str, Table] = {}
        for group in document.get("groups", []):
            for table_doc in group.get("tables", []):
                table = Table(table_doc)
                if table.name in self._tables:
                    raise StoreError(f"table {table.name} appears twice in the schema")
                self._tables[table.name] = table

    @classmethod
    def open(cls, path) -> "Store":
        path = Path(path)
        if not path.is_file():
            raise StoreError(f"no store at {path}")
        # mode=rw: an existing file only; sqlite3.connect would otherwise create an empty one.
        con = sqlite3.connect(f"{path.resolve().as_uri()}?mode=rw", uri=True, isolation_level=None, timeout=30.0)
        try:
            con.execute("PRAGMA foreign_keys = ON")
            con.execute("PRAGMA trusted_schema = OFF")
            con.execute("PRAGMA busy_timeout = 30000")
            application_id = con.execute("PRAGMA application_id").fetchone()[0] & 0xFFFFFFFF
            user_version = con.execute("PRAGMA user_version").fetchone()[0]
            try:
                found = con.execute("SELECT value FROM store_info WHERE key = 'schema_json'").fetchone()
            except sqlite3.DatabaseError as exc:
                raise StoreError(f"{path}: no store_info table ({exc})") from exc
            if found is None or not isinstance(found[0], str):
                raise StoreError(f"{path}: no store_info('schema_json') row")
            try:
                document = json.loads(found[0])
            except ValueError as exc:
                raise StoreError(f"{path}: schema_json is not JSON") from exc
            if not isinstance(document, dict) or document.get("schema") != SCHEMA_ID:
                raise StoreError(f"{path}: schema_json is not {SCHEMA_ID}")
            if document.get("user_version") != user_version:
                raise StoreError(f"{path}: user_version {user_version} != schema {document.get('user_version')}")
            if document.get("application_id") != application_id:
                raise StoreError(f"{path}: application_id {application_id:#010x} != schema "
                                 f"{document.get('application_id')!r}")
            return cls(con, path, found[0], document)
        except BaseException:
            con.close()
            raise

    # -- plumbing ---------------------------------------------------------------------------------------------------
    @property
    def connection(self) -> sqlite3.Connection:
        """The underlying connection (diagnostics and tests; writes go through the methods)."""
        return self._con

    @property
    def tables(self) -> list:
        return list(self._tables)

    def table(self, name: str) -> Table:
        try:
            return self._tables[name]
        except KeyError:
            raise StoreError(f"no table {name!r} in this store's schema") from None

    def close(self) -> None:
        self._con.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @contextlib.contextmanager
    def transaction(self):
        """``BEGIN IMMEDIATE`` ... ``COMMIT``; ``ROLLBACK`` and re-raise on any exception."""
        if self._con.in_transaction:
            raise StoreError("a transaction is already open on this store")
        self._con.execute("BEGIN IMMEDIATE")
        try:
            yield self
        except BaseException:
            self._con.execute("ROLLBACK")
            raise
        self._con.execute("COMMIT")

    # -- rows -------------------------------------------------------------------------------------------------------
    def insert(self, table: str, row: dict) -> None:
        t = self.table(table)
        self._con.execute(t.insert_sql, t.row_values(row))

    def upsert(self, table: str, row: dict) -> None:
        t = self.table(table)
        self._con.execute(t.upsert_sql, t.row_values(row))

    def get(self, table: str, key: dict) -> dict | None:
        t = self.table(table)
        where = " AND ".join(f"{k} = ?" for k in t.key)
        found = self._con.execute(f"{t.select_sql} WHERE {where}", t.key_values(key)).fetchone()
        return None if found is None else t.to_row(found)

    def select(self, table: str, where: dict | None = None) -> list:
        """Rows matching every ``column: value`` of ``where`` (``None`` matches NULL), in primary-key order."""
        t = self.table(table)
        clauses, values = [], []
        for name, value in (where or {}).items():
            if name not in t.by_name:
                raise StoreError(f"{t.name}: unknown column {name!r}")
            if value is None:
                clauses.append(f"{name} IS NULL")
            else:
                clauses.append(f"{name} = ?")
                values.append(t.to_sql(t.by_name[name], value))
        sql = t.select_sql + (" WHERE " + " AND ".join(clauses) if clauses else "") + t.order_sql
        return [t.to_row(r) for r in self._con.execute(sql, values).fetchall()]
