"""Research-store fixtures on Python's SQLite (P9 SQL1, synthetic): every group fixture's DDL applies on the host
SQLite (3.43.1, older than the vendored 3.53.2), its CHECKs refuse bad values, make_py_fixture.py reproduces the
committed py_created.sqlite, and digest_oracle.py reproduces golden_digests.json.

Run: python -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_store_fixtures.py
"""
from __future__ import annotations

import json
import shutil
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

FIXTURES = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "research_store"
sys.path.insert(0, str(FIXTURES))
import digest_oracle  # noqa: E402
import make_py_fixture  # noqa: E402
import make_store  # noqa: E402

GROUPS = ("catalog_core", "cache", "toy")
SHA = "a" * 64


class FixtureDdlTest(unittest.TestCase):
    def test_host_sqlite_supports_the_grammar(self):
        # STRICT tables (3.37) and the built-in JSON functions (3.38); nothing newer is used.
        self.assertGreaterEqual(sqlite3.sqlite_version_info, (3, 38, 0))

    def test_every_fixture_ddl_statement_applies(self):
        for name in GROUPS:
            group = make_store.load_group(name)
            con = sqlite3.connect(":memory:")
            try:
                for table in group["tables"]:
                    for statement in table["ddl"]:
                        con.execute(statement)
                    info = con.execute(f"PRAGMA table_info({table['name']})").fetchall()
                    self.assertEqual([row[1] for row in info], [c["name"] for c in table["columns"]])
                    self.assertEqual([bool(row[3]) for row in info], [not c["nullable"] for c in table["columns"]])
                    keyed = sorted((row[5], row[1]) for row in info if row[5])
                    self.assertEqual([n for _, n in keyed], table["key"])
                    strict = con.execute("SELECT strict, wr FROM pragma_table_list WHERE name = ?",
                                         (table["name"],)).fetchone()
                    self.assertEqual(strict, (1, 1), table["name"])  # STRICT, WITHOUT ROWID
            finally:
                con.close()

    def test_groups_print_in_the_documented_key_order(self):
        for name in GROUPS:
            group = make_store.load_group(name)
            self.assertEqual(list(group), ["name", "db", "version", "tables", "views"])
            for table in group["tables"]:
                self.assertEqual(list(table), ["name", "version", "since", "append_only", "volatile", "key",
                                               "columns", "ddl"])
                for column in table["columns"]:
                    self.assertEqual(list(column), ["name", "type", "nullable", "indexed", "volatile", "since",
                                                    "allowed"])
            text = (make_store.SCHEMA_DIR / f"{name}.json").read_bytes()
            self.assertNotIn(b"\r", text)
            self.assertEqual(text, (json.dumps(group, indent=2) + "\n").encode("ascii"))


class FixtureChecksTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.con = None

    def tearDown(self):
        if self.con is not None:
            self.con.close()
        self.tmp.cleanup()

    def open(self, db_kind: str, group: str) -> sqlite3.Connection:
        path = make_store.build(Path(self.tmp.name) / f"{group}.sqlite", db_kind, [group])
        self.con = sqlite3.connect(str(path), isolation_level=None)
        return self.con

    def toy(self, **changes) -> tuple:
        row = {"k": "k", "i": 1, "b": 1, "r": 0.5, "u": -1, "s": SHA, "p": "a/b.json", "j": "{}", "x": b"\x00",
               "o": None, "v": None}
        row.update(changes)
        return tuple(row.values())

    def test_checks_refuse_bad_sha256_relpath_bool_json_and_allowed_values(self):
        con = self.open("cache", "toy")
        insert = "INSERT INTO toy(k, i, b, r, u, s, p, j, x, o, v) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
        con.execute(insert, self.toy(k="ok", o="red"))
        bad = [dict(s="A" * 64), dict(s="a" * 63), dict(s="g" * 64), dict(p="/abs"), dict(p="a\\b"), dict(p=".."),
               dict(p="a/../b"), dict(p="a/.."), dict(b=2), dict(j="{nope"), dict(o="green"), dict(i="one"),
               dict(x="text-not-blob")]
        for change in bad:
            with self.subTest(change=change), self.assertRaises(sqlite3.IntegrityError):
                con.execute(insert, self.toy(k="bad", **change))
        self.assertEqual(con.execute("SELECT count(*) FROM toy").fetchone(), (1,))
        con.execute(insert, self.toy(k="look-alike", p="a/..b/c..", o="blue"))
        with self.assertRaises(sqlite3.IntegrityError):  # append-only
            con.execute("UPDATE toy SET i = 2")
        with self.assertRaises(sqlite3.IntegrityError):
            con.execute("DELETE FROM toy")

    def test_core_and_cache_checks(self):
        con = self.open("catalog", "catalog_core")
        insert = ("INSERT INTO artifact(path_key, path, sha256, bytes, class, json_schema, sha_source, declared_by, "
                  "eol, producer_key) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
        con.execute(insert, ("a", "a", SHA, None, "spec", None, "verified", None, "crlf", None))
        for row in [("b", "b", SHA, None, "spec", None, "guessed", None, None, None),
                    ("c", "c", SHA, None, "spec", None, "declared", None, "cr", None),
                    ("d", "d", SHA, None, "spec", None, "declared", "/abs/manifest.json", None, None)]:
            with self.subTest(row=row), self.assertRaises(sqlite3.IntegrityError):
                con.execute(insert, row)
        con.close()
        con = self.open("cache", "cache")
        insert = ("INSERT INTO record(root, kind, key_sha256, key, body, body_file, body_bytes, content_sha256) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?)")
        con.execute(insert, ("", "factor", SHA, "{}", "{}", None, 2, SHA))
        for body, body_file in [(None, None), ("{}", "objects/aa/x.json")]:  # exactly one of the two
            with self.subTest(body=body, body_file=body_file), self.assertRaises(sqlite3.IntegrityError):
                con.execute(insert, ("", "aim", SHA, "{}", body, body_file, 2, SHA))


class PyFixtureTest(unittest.TestCase):
    def test_make_py_fixture_reproduces_the_committed_logical_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            rebuilt = make_py_fixture.build(Path(tmp) / "rebuilt.sqlite")
            committed = Path(tmp) / "committed.sqlite"  # a copy: opening the fixture in place would add -shm
            shutil.copyfile(make_py_fixture.FIXTURE, committed)
            expected = make_py_fixture.logical_content(committed)
            self.assertEqual(make_py_fixture.logical_content(rebuilt), expected)
        self.assertEqual(expected["pragmas"], {"application_id": 0x41545843, "user_version": 1,
                                               "page_size": 8192, "journal_mode": "wal"})
        schema_row = [v for k, v in expected["rows"]["store_info"] if k == "schema_json"]
        self.assertEqual(schema_row, [make_store.schema_text("catalog", [make_store.load_group("catalog_core")])])


class DigestOracleTest(unittest.TestCase):
    def test_oracle_reproduces_the_golden_digests(self):
        rows = json.loads((FIXTURES / "golden_rows.json").read_text(encoding="utf-8"))
        golden_text = (FIXTURES / "golden_digests.json").read_text(encoding="utf-8")
        golden = json.loads(golden_text)
        self.assertEqual(digest_oracle.digests(rows), golden)
        self.assertEqual(list(golden), [row["name"] for row in rows])
        self.assertEqual(golden_text, json.dumps(golden, indent=2) + "\n")

    def test_encoding_literal(self):
        row = {"table": "reals", "version": 1, "columns": [{"name": "r", "type": "real", "bits": "8000000000000000"},
                                                           {"name": "t", "type": "text", "value": "é"},
                                                           {"name": "n", "type": "int", "value": None}]}
        self.assertEqual(digest_oracle.encode(row),
                         b"atx.record-digest/v1\nrow reals@1\nr=r8000000000000000\nt=t2:\xc3\xa9\nn=~\n")


if __name__ == "__main__":
    unittest.main()
