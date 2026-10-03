"""research_store: the generic accessor on stores made by the test-only make_store.py (P9 SQL1, synthetic).

Run: python -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_store.py
"""
from __future__ import annotations

import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

import research_store as rs

FIXTURES = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "research_store"
sys.path.insert(0, str(FIXTURES))
import make_store  # noqa: E402

SHA = "a" * 64
U64_MAX = (1 << 64) - 1


def toy_row(key: str = "k1", **changes) -> dict:
    row = {"k": key, "i": -42, "b": True, "r": 1.5, "u": U64_MAX, "s": SHA, "p": "dir/file.json",
           "j": '{"x":[1,2]}', "x": b"\x00\xff\x10", "o": None, "v": 7}
    row.update(changes)
    return row


class StoreCase(unittest.TestCase):
    group = "toy"
    db_kind = "cache"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = make_store.build(Path(self.tmp.name) / "store.sqlite", self.db_kind, [self.group])
        self.store = rs.Store.open(self.path)

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()

    def raw(self, sql: str, *args):
        con = sqlite3.connect(str(self.path), isolation_level=None)
        try:
            return con.execute(sql, args).fetchall()
        finally:
            con.close()


class OpenTest(StoreCase):
    def test_open_reads_the_schema_row_and_sets_the_policy_pragmas(self):
        self.assertEqual(self.store.tables, ["store_info", "toy", "plain"])
        self.assertEqual(self.store.schema_text, make_store.schema_text("cache", [make_store.load_group("toy")]))
        con = self.store.connection
        self.assertEqual(con.execute("PRAGMA foreign_keys").fetchone(), (1,))
        self.assertEqual(con.execute("PRAGMA trusted_schema").fetchone(), (0,))
        self.assertEqual(con.execute("PRAGMA busy_timeout").fetchone(), (30000,))
        self.assertEqual(con.execute("PRAGMA journal_mode").fetchone(), ("wal",))
        self.assertIsNone(con.isolation_level)

    def test_open_runs_no_ddl_and_never_creates_a_file(self):
        before = self.raw("SELECT type, name, sql FROM sqlite_schema ORDER BY name")
        self.store.insert("toy", toy_row())
        self.store.close()
        self.store = rs.Store.open(self.path)
        self.assertEqual(self.raw("SELECT type, name, sql FROM sqlite_schema ORDER BY name"), before)
        self.assertEqual(self.raw("PRAGMA user_version"), [(1,)])
        missing = Path(self.tmp.name) / "missing.sqlite"
        with self.assertRaises(rs.StoreError):
            rs.Store.open(missing)
        self.assertFalse(missing.exists())

    def test_open_refuses_a_store_that_does_not_match_its_schema(self):
        self.store.close()
        self.raw("PRAGMA user_version = 2")
        with self.assertRaisesRegex(rs.StoreError, "user_version"):
            rs.Store.open(self.path)
        self.raw("PRAGMA user_version = 1")
        self.raw(f"PRAGMA application_id = {0x41545843}")  # a catalog id on a cache store
        with self.assertRaisesRegex(rs.StoreError, "application_id"):
            rs.Store.open(self.path)
        self.raw(f"PRAGMA application_id = {0x4154584B}")
        self.raw("UPDATE store_info SET value = '{\"schema\": \"other/v1\"}' WHERE key = 'schema_json'")
        with self.assertRaisesRegex(rs.StoreError, "atx.store-schema/v1"):
            rs.Store.open(self.path)
        self.raw("DELETE FROM store_info WHERE key = 'schema_json'")
        with self.assertRaisesRegex(rs.StoreError, "schema_json"):
            rs.Store.open(self.path)
        plain = Path(self.tmp.name) / "plain.sqlite"
        con = sqlite3.connect(str(plain))
        con.execute("CREATE TABLE x(a)")
        con.close()
        with self.assertRaisesRegex(rs.StoreError, "store_info"):
            rs.Store.open(plain)
        self.store = rs.Store.open(make_store.build(Path(self.tmp.name) / "again.sqlite", "cache", ["toy"]))


class RowsTest(StoreCase):
    def test_every_type_round_trips(self):
        row = toy_row(o="red", v=None, r=-2.75e-300, i=-(1 << 63), b=False, x=b"")
        self.store.insert("toy", row)
        self.assertEqual(self.store.get("toy", {"k": "k1"}), row)
        self.assertEqual(self.store.select("toy"), [row])
        # u64 is stored as its signed 64-bit pattern; bool as 0 / 1; blob as BLOB.
        self.assertEqual(self.raw("SELECT u, b, typeof(x) FROM toy"), [(-1, 0, "blob")])
        self.assertIsNone(self.store.get("toy", {"k": "absent"}))

    def test_upsert_get_and_select_with_where(self):
        self.store.upsert("plain", {"a": "x", "b": 1, "c": 0.5, "d": "first"})
        self.store.upsert("plain", {"a": "x", "b": 2, "c": None, "d": "other"})
        self.store.upsert("plain", {"a": "x", "b": 1, "c": None, "d": "second"})
        self.assertEqual(self.store.get("plain", {"a": "x", "b": 1}), {"a": "x", "b": 1, "c": None, "d": "second"})
        self.assertEqual([r["b"] for r in self.store.select("plain", {"a": "x"})], [1, 2])
        self.assertEqual([r["d"] for r in self.store.select("plain", {"c": None})], ["second", "other"])
        self.assertEqual(self.store.select("plain", {"d": "nothing"}), [])

    def test_select_orders_by_key_under_reverse_unordered_selects(self):
        self.store.connection.execute("PRAGMA reverse_unordered_selects = ON")
        for a, b in [("b", 2), ("a", 9), ("b", -1), ("a", 0), ("c", 5)]:
            self.store.insert("plain", {"a": a, "b": b, "c": None, "d": "-"})
        self.assertEqual([(r["a"], r["b"]) for r in self.store.select("plain")],
                         [("a", 0), ("a", 9), ("b", -1), ("b", 2), ("c", 5)])
        self.assertEqual([r["b"] for r in self.store.select("plain", {"a": "b"})], [-1, 2])

    def test_type_refusals_write_nothing(self):
        bad = [dict(i=1.0), dict(i=True), dict(i=1 << 63), dict(u=-1), dict(u=1 << 64), dict(b=1), dict(r=1),
               dict(r=float("nan")), dict(r=-0.0), dict(s=b"x"), dict(x="text"), dict(x=bytearray(b"x")),
               dict(k=None), dict(i=None), dict(extra=1)]
        for change in bad:
            with self.subTest(change=change), self.assertRaises(rs.StoreError):
                self.store.insert("toy", toy_row(**change))
        with self.assertRaises(rs.StoreError):
            self.store.insert("toy", {"k": "only-key"})  # a missing required column is None
        with self.assertRaises(rs.StoreError):
            self.store.get("plain", {"a": "x"})  # a partial key
        with self.assertRaises(rs.StoreError):
            self.store.insert("nope", {})
        with self.assertRaises(rs.StoreError):
            self.store.select("plain", {"zzz": 1})
        self.assertEqual(self.raw("SELECT count(*) FROM toy"), [(0,)])

    def test_sqlite_checks_and_append_only_surface_as_integrity_errors(self):
        self.store.insert("toy", toy_row())
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.insert("toy", toy_row())  # duplicate key
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.upsert("toy", toy_row(i=5))  # append-only: the UPDATE path raises
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.insert("toy", toy_row("k2", s="A" * 64))
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.insert("toy", toy_row("k3", o="green"))
        self.assertEqual(self.store.get("toy", {"k": "k1"})["i"], -42)

    def test_transaction_commits_or_rolls_back(self):
        with self.store.transaction():
            self.store.insert("plain", {"a": "t", "b": 1, "c": None, "d": "kept"})
        with self.assertRaises(RuntimeError):
            with self.store.transaction():
                self.store.insert("plain", {"a": "t", "b": 2, "c": None, "d": "dropped"})
                raise RuntimeError("abort")
        self.assertEqual([r["d"] for r in self.store.select("plain")], ["kept"])
        with self.store.transaction():
            with self.assertRaises(rs.StoreError):
                with self.store.transaction():
                    pass


class CatalogStoreTest(StoreCase):
    group = "catalog_core"
    db_kind = "catalog"

    def test_catalog_tables_from_the_schema(self):
        self.assertEqual(self.store.tables,
                         ["store_info", "catalog_run", "artifact", "artifact_seen", "skipped_path", "producer"])
        row = {"path_key": "a.json", "path": "a.json", "sha256": SHA, "bytes": 3, "class": "spec",
               "json_schema": None, "sha_source": "verified", "declared_by": None, "eol": "lf",
               "producer_key": None}
        self.store.insert("artifact", row)
        self.assertEqual(self.store.get("artifact", {"path_key": "a.json"}), row)
        self.assertEqual(self.store.connection.execute("PRAGMA application_id").fetchone(), (0x41545843,))


if __name__ == "__main__":
    unittest.main()
