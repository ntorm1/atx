"""record_store: content-keyed JSON records, verified on read, safe to delete (synthetic).

Run: python -m pytest -q -p no:cacheprovider atx-engine/tools/test_record_store.py
"""
import json
from pathlib import Path
import shutil
import tempfile
import unittest

import record_store as rs

KEY = {"role_manifest_sha256": "ab" * 32, "producer_fingerprint": "cd" * 32, "payload_sha256": "ef" * 32}
BODY = {"z": [1.0, None, -0.0, 1e-300], "a": {"small": 1, "mid": 2, "large": 3}, "n": 7}


class RecordStoreTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = rs.RecordStore(Path(self.tmp.name) / "root")

    def tearDown(self):
        self.tmp.cleanup()

    def test_round_trip_keeps_values_and_key_order(self):
        self.assertIsNone(self.store.get("factor", KEY))
        self.assertTrue(self.store.put("factor", KEY, BODY))
        got = self.store.get("factor", KEY)
        self.assertEqual(got, BODY)
        self.assertEqual(list(got), ["z", "a", "n"])                   # the body keeps its key order
        self.assertEqual(list(got["a"]), ["small", "mid", "large"])
        self.assertEqual(repr(got["z"][2]), "-0.0")
        path = self.store.path("factor", KEY)
        self.assertEqual(path.name, f"{rs.key_sha256('factor', KEY)}.json")
        self.assertEqual(sorted(p.name for p in path.parent.iterdir()), [path.name])  # no stray partial file

    def test_other_key_kind_tamper_and_deletion_are_misses(self):
        self.store.put("factor", KEY, BODY)
        self.assertIsNone(self.store.get("aim", KEY))
        other = dict(KEY, payload_sha256="00" * 32)
        # a foreign record at this key's path (e.g. a colliding directory) carries its own key: a miss
        self.store.path("factor", other).parent.mkdir(parents=True, exist_ok=True)
        self.store.path("factor", other).write_bytes(self.store.path("factor", KEY).read_bytes())
        self.assertIsNone(self.store.get("factor", other))
        j = json.loads(self.store.path("factor", KEY).read_bytes())
        j["body"]["n"] = 8
        self.store.path("factor", KEY).write_text(json.dumps(j), encoding="utf-8")
        self.assertIsNone(self.store.get("factor", KEY))
        self.store.path("factor", KEY).write_text("NaN", encoding="utf-8")
        self.assertIsNone(self.store.get("factor", KEY))
        shutil.rmtree(self.store.root)
        self.assertIsNone(self.store.get("factor", KEY))
        self.assertTrue(self.store.put("factor", KEY, BODY))  # recreated on demand
        self.assertEqual(self.store.get("factor", KEY), BODY)

    def test_non_finite_body_is_refused_and_content_hash_ignores_order(self):
        with self.assertRaises(ValueError):
            self.store.put("factor", KEY, {"x": float("nan")})
        reordered = {"n": 7, "a": {"large": 3, "mid": 2, "small": 1}, "z": BODY["z"]}
        self.assertEqual(rs.content_sha256("factor", KEY, reordered), rs.content_sha256("factor", KEY, BODY))


if __name__ == "__main__":
    unittest.main()
