"""--reuse keys added by P9 lane A1: interpreter / library versions (FD-2) and the producer kind (K-P9-3, ruling P6).

* every manifest records ``runtime_versions`` (python, numpy, pyarrow, duckdb);
* a prior that records other versions has no field reused; a prior that records none (written before P9) is reused
  under the other rules and the reuse block says so (``prior_runtime_versions`` null);
* a prior entry whose ``producer.kind`` is ``engine`` is never reused by the Python rule (builder group and field
  module alike); a producer without ``kind`` is the legacy Python shape.
Synthetic data only (the slice-1 fixture's role and FINRA inputs).
"""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

import prepare_research_fields as tool

FIXTURE = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "research_fields"
sys.path.insert(0, str(FIXTURE))
import make_research_fields_fixture as fx  # noqa: E402  (the slice-1 synthetic inputs)

FIELDS = ["si_shares", "vol_126"]   # a builder group (finra) and a field module group (price_volume)


class ReuseKeys(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.base = Path(cls.temp.name)
        cls.role_sha = fx.write_role(cls.base / "role")
        fx.write_finra(cls.base / "finra")
        cls.prior = cls.build("prior")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def build(cls, out, **kw):
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(cls.base / "role", cls.role_sha, cls.base / out, FIELDS, finra=cls.base / "finra",
                            module_options={}, **kw)

    def edited_prior(self, name, edit):
        """A copy of the prior directory whose manifest ``edit`` changes (no --reuse-sha256 is passed)."""
        target = self.base / name
        shutil.copytree(self.base / "prior", target)
        m = json.loads((target / "manifest.json").read_bytes())
        edit(m)
        (target / "manifest.json").write_bytes((json.dumps(m, sort_keys=True, indent=2) + "\n").encode("utf-8"))
        return target

    def test_the_manifest_records_the_runtime(self):
        rt = self.prior["runtime_versions"]
        self.assertEqual(rt, tool.runtime_versions())
        self.assertEqual(set(rt), {"python", "numpy", "pyarrow", "duckdb"})
        self.assertEqual(rt["numpy"], tool.np.__version__)
        again = self.build("again", reuse=self.base / "prior")
        self.assertEqual(again["reuse"]["reused"], FIELDS)
        self.assertEqual(again["reuse"]["prior_runtime_versions"], rt)
        self.assertEqual(again["reuse"]["runtime_rule"], tool.REUSE_RUNTIME_RULE)

    def test_other_versions_reuse_nothing(self):
        def older_numpy(m):
            m["runtime_versions"]["numpy"] = "0.0.1"
        moved = self.build("moved", reuse=self.edited_prior("prior-numpy", older_numpy))
        self.assertEqual(moved["reuse"]["reused"], [])
        for name in FIELDS:
            self.assertIn(f"runtime versions differ (numpy 0.0.1 -> {tool.np.__version__})",
                          moved["reuse"]["not_reused"][name])
            self.assertEqual((self.base / "moved" / f"{name}.f64").read_bytes(),
                             (self.base / "prior" / f"{name}.f64").read_bytes())   # recomputed, same bytes

    def test_a_prior_without_the_record_is_reused_and_named(self):
        def legacy(m):
            del m["runtime_versions"]
        old = self.build("old", reuse=self.edited_prior("prior-legacy", legacy))
        self.assertEqual(old["reuse"]["reused"], FIELDS)
        self.assertIsNone(old["reuse"]["prior_runtime_versions"])

    def test_engine_produced_entries_are_not_reused_by_python(self):
        def engine(m):
            for e in m["fields"]:
                e["producer"] = {"kind": "engine", "exe_sha256": "1" * 64, "git_sha": "2" * 40,
                                 "build_type": "Release", "receipt_sha256": "3" * 64}
        got = self.build("from-engine", reuse=self.edited_prior("prior-engine", engine))
        self.assertEqual(got["reuse"]["reused"], [])
        self.assertEqual(got["reuse"]["not_reused"], {x: tool.ENGINE_REUSE_REASON for x in FIELDS})
        self.assertTrue(tool.engine_produced({"producer": {"kind": "engine"}}))
        self.assertFalse(tool.engine_produced({"producer": {"module": "research_fields_price.py"}}))   # P6 legacy
        self.assertFalse(tool.engine_produced({}))


if __name__ == "__main__":
    unittest.main()
