"""record_store with a SQLite cache index (P9 SQL1, synthetic): selection by file presence, round trips in both
partitions, verification misses, read-through import, large bodies, concurrent writers, failures.

The index files are made by the test-only make_store.py (production: ``atx-research-store cache init``, SQL2).
Run: python -m pytest -q -p no:cacheprovider atx-engine/tools/test_record_store_sqlite.py
"""
from __future__ import annotations

import contextlib
import io
import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import record_store as rs

FIXTURES = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "research_store"
sys.path.insert(0, str(FIXTURES))
import make_store  # noqa: E402

KEY = {"role_manifest_sha256": "ab" * 32, "producer_fingerprint": "cd" * 32, "payload_sha256": "ef" * 32}
BODY = {"z": [1.0, None, -0.0, 1e-300], "a": {"small": 1, "mid": 2, "large": 3}, "n": 7}
TOOLS = Path(__file__).resolve().parent


def make_index(directory: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    return make_store.build(directory / rs.INDEX_NAME, "cache", ["cache"])


def rows(index: Path) -> list:
    con = sqlite3.connect(str(index))
    try:
        return con.execute("SELECT root, kind, key_sha256, key, body, body_file, body_bytes, content_sha256 "
                           "FROM record ORDER BY root, kind, key_sha256").fetchall()
    finally:
        con.close()


def execute(index: Path, sql: str, *args) -> None:
    con = sqlite3.connect(str(index), isolation_level=None)
    try:
        con.execute(sql, args)
    finally:
        con.close()


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.err = io.StringIO()

    def tearDown(self):
        rs.close_indexes()
        self.tmp.cleanup()

    def store(self, root: Path) -> rs.RecordStore:
        with contextlib.redirect_stderr(self.err):
            return rs.RecordStore(root)


class NoIndexTest(Case):
    def test_no_index_file_keeps_todays_files_bytes_and_silence(self):
        root = self.dir / "work" / "abc-w"
        store = self.store(root)
        with contextlib.redirect_stderr(self.err):
            self.assertTrue(store.put("factor", KEY, BODY))
            self.assertEqual(store.get("factor", KEY), BODY)
        path = store.path("factor", KEY)
        expected = {"schema": rs.SCHEMA, "kind": "factor", "key": KEY, "body": BODY,
                    "content_sha256": rs.content_sha256("factor", KEY, BODY)}
        self.assertEqual(path.read_bytes(), json.dumps(expected, separators=(",", ":")).encode("utf-8"))
        self.assertEqual(sorted(p.relative_to(self.dir).as_posix() for p in self.dir.rglob("*") if p.is_file()),
                         [path.relative_to(self.dir).as_posix()])
        self.assertEqual(self.err.getvalue(), "")
        self.assertIsNone(store._index)


class IndexTest(Case):
    def test_round_trip_in_the_root_partition(self):
        root = self.dir / "store"
        index = make_index(root)
        store = self.store(root)
        self.assertTrue(store.put("factor", KEY, BODY))
        got = store.get("factor", KEY)
        self.assertEqual(got, BODY)
        self.assertEqual(list(got), ["z", "a", "n"])
        self.assertEqual(repr(got["z"][2]), "-0.0")
        self.assertFalse(store.path("factor", KEY).exists())  # the DB only: no JSON record file
        (row,) = rows(index)
        self.assertEqual(row[:3], ("", "factor", rs.key_sha256("factor", KEY)))
        self.assertEqual(row[3], rs.canonical(KEY).decode("utf-8"))
        self.assertEqual(row[4], json.dumps(BODY, separators=(",", ":")))  # compact, insertion order
        self.assertEqual((row[5], row[6], row[7]), (None, len(row[4]), rs.content_sha256("factor", KEY, BODY)))

    def test_round_trip_in_the_parent_partition_keeps_body_key_order(self):
        index = make_index(self.dir / "work")
        store = self.store(self.dir / "work" / "abc-w")
        body = {"zeta": 1, "alpha": {"y": 2, "b": [3.5]}, "mid": None}
        self.assertTrue(store.put("aim", KEY, body))
        self.assertTrue(store.put("aim", KEY, body))  # the same content again: True, one row
        got = store.get("aim", KEY)
        self.assertEqual(list(got), ["zeta", "alpha", "mid"])
        self.assertEqual(list(got["alpha"]), ["y", "b"])
        self.assertEqual([r[:2] for r in rows(index)], [("abc-w", "aim")])
        self.assertIsNone(self.store(self.dir / "work" / "other-w").get("aim", KEY))  # another partition

    def test_key_mismatch_and_content_tamper_are_misses(self):
        index = make_index(self.dir / "store")
        store = self.store(self.dir / "store")
        self.assertTrue(store.put("factor", KEY, BODY))
        execute(index, "UPDATE record SET key = ?", json.dumps(dict(KEY, payload_sha256="00" * 32)))
        self.assertIsNone(store.get("factor", KEY))
        execute(index, "UPDATE record SET key = ?, body = ?", rs.canonical(KEY).decode(),
                json.dumps(dict(BODY, n=8), separators=(",", ":")))
        self.assertIsNone(store.get("factor", KEY))
        execute(index, "UPDATE record SET body = ?", json.dumps(BODY, separators=(",", ":")))
        self.assertEqual(store.get("factor", KEY), BODY)
        execute(index, "UPDATE record SET content_sha256 = ?", "0" * 64)
        self.assertIsNone(store.get("factor", KEY))
        self.assertIsNone(store.get("factor", dict(KEY, payload_sha256="11" * 32)))

    def test_read_through_imports_a_valid_legacy_record(self):
        root = self.dir / "work" / "abc-w"
        self.assertTrue(rs.RecordStore(root).put("factor", KEY, BODY))  # written before any index existed
        legacy = rs.RecordStore(root).path("factor", KEY)
        tampered_key = dict(KEY, payload_sha256="22" * 32)
        self.assertTrue(rs.RecordStore(root).put("factor", tampered_key, BODY))
        bad = rs.RecordStore(root).path("factor", tampered_key)
        record = json.loads(bad.read_bytes())
        record["body"]["n"] = 9
        bad.write_text(json.dumps(record), encoding="utf-8")
        index = make_index(self.dir / "work")
        store = self.store(root)
        self.assertEqual(store.get("factor", KEY), BODY)
        self.assertIsNone(store.get("factor", tampered_key))  # today's checks: not imported
        self.assertEqual([(r[0], r[2]) for r in rows(index)], [("abc-w", rs.key_sha256("factor", KEY))])
        legacy.unlink()
        self.assertEqual(self.store(root).get("factor", KEY), BODY)

    def test_a_body_over_one_mib_goes_to_objects(self):
        index = make_index(self.dir / "store")
        store = self.store(self.dir / "store")
        body = {"series": [0.123456789] * 200_000}  # ~2.2 MB compact
        content = rs.content_sha256("factor", KEY, body)
        self.assertTrue(store.put("factor", KEY, body))
        (row,) = rows(index)
        relative = f"objects/{content[:2]}/{content}.json"
        self.assertEqual((row[4], row[5]), (None, relative))
        data = (self.dir / "store" / relative).read_bytes()
        self.assertGreater(len(data), 2 * 1024 * 1024)
        self.assertEqual((row[6], data), (len(data), json.dumps(body, separators=(",", ":")).encode()))
        self.assertEqual(store.get("factor", KEY), body)
        self.assertEqual(sorted(p.name for p in (self.dir / "store" / "objects" / content[:2]).iterdir()),
                         [f"{content}.json"])  # no stray partial
        (self.dir / "store" / relative).write_bytes(data[:-1] + b" ")  # same size, other bytes: hash miss
        self.assertIsNone(store.get("factor", KEY))
        (self.dir / "store" / relative).write_bytes(data[:-1])  # other size: miss before parsing
        self.assertIsNone(store.get("factor", KEY))

    def test_write_failures_return_false(self):
        index = make_index(self.dir / "store")
        store = self.store(self.dir / "store")
        (self.dir / "store" / "objects").write_bytes(b"a file where the objects directory belongs")
        self.assertFalse(store.put("factor", KEY, {"series": [0.5] * 400_000}))
        self.assertEqual(rows(index), [])
        self.assertTrue(store.put("factor", KEY, BODY))
        self.assertFalse(store.put("factor", KEY, dict(BODY, n=8)))  # same key, other content
        self.assertEqual(store.get("factor", KEY), BODY)
        with self.assertRaises(ValueError):  # unchanged: a non-finite body is refused by the caller's input
            store.put("factor", KEY, {"x": float("nan")})
        rs.close_indexes()
        broken = self.dir / "broken"
        broken.mkdir()
        (broken / rs.INDEX_NAME).write_bytes(b"not a database")
        broken_store = self.store(broken)
        self.assertFalse(broken_store.put("factor", KEY, BODY))
        self.assertIsNone(broken_store.get("factor", KEY))

    def test_the_stderr_line_is_printed_once_per_index(self):
        index = make_index(self.dir / "work")
        for name in ("a-w", "b-w", "a-w"):
            self.store(self.dir / "work" / name)
        self.assertEqual(self.err.getvalue(), f"record_store: index sqlite {index.resolve().as_posix()}\n")

    def test_two_processes_put_concurrently_with_no_loss(self):
        index = make_index(self.dir / "work")
        script = (
            "import sys\n"
            f"sys.path.insert(0, {str(TOOLS)!r})\n"
            "import record_store as rs\n"
            "store = rs.RecordStore(sys.argv[1])\n"
            "ok = all(store.put('factor', {'worker': sys.argv[2], 'i': i}, {'i': i, 'w': [0.5] * 50})\n"
            "         for i in range(60))\n"
            "sys.exit(0 if ok else 3)\n")
        procs = [subprocess.Popen([sys.executable, "-c", script, str(self.dir / "work" / "abc-w"), worker],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) for worker in ("a", "b")]
        try:
            results = [p.communicate(timeout=120) for p in procs]
        finally:
            for p in procs:  # PY-HYG: never leave a child behind
                if p.poll() is None:
                    p.kill()
                    p.wait()
        self.assertEqual([p.returncode for p in procs], [0, 0], results)
        self.assertEqual(len(rows(index)), 120)
        for _, err in results:  # each process announced its selection once
            self.assertEqual(err.decode().count("record_store: index sqlite"), 1)


if __name__ == "__main__":
    unittest.main()
