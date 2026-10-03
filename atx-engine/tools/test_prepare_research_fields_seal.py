"""The research seal on the builder's read and publish sides, under the repository window (P9 lane A1, FD-5, G-P9).

This directory's conftest.py binds the superseded window for the legacy-dated fixtures (ruling A1-C: removing that
bind and re-dating 25 modules is wave-2 lane T2's), so every check here runs in a fresh interpreter that never loads
it, on the slice-1 synthetic inputs (sessions 2021-2022, one FINRA row disseminated 2024-02-07):
* the manifest's seal block is the repository seal and the sealed row is published as ``rows_sealed_dropped``;
* ``--reuse`` refuses a prior manifest written under another seal (``load_prior``), before any payload is copied;
* a prior without a seal block is accepted as legacy and logged (ruling P13: refused from wave 2).
"""
from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from test_research_window import isolated

TOOLS = Path(__file__).resolve().parent
FIXTURE = TOOLS.parents[0] / "tests" / "fixtures" / "research_fields"
CODE = r"""
import contextlib, io, json, shutil, sys
from pathlib import Path
sys.path.insert(0, sys.argv[2])
import make_research_fields_fixture as fx
import prepare_research_fields as t, research_window as rw
base = Path(sys.argv[1])
sha = fx.write_role(base / "role")
fx.write_finra(base / "finra")
fields = ["si_shares", "si_dtc"]

def build(out, **kw):
    with contextlib.redirect_stdout(io.StringIO()):
        return t.run(base / "role", sha, base / out, fields, finra=base / "finra", module_options={}, **kw)

def edited(name, edit):
    shutil.copytree(base / "prior", base / name)
    p = base / name / "manifest.json"
    d = json.loads(p.read_bytes())
    edit(d)
    p.write_text(json.dumps(d, indent=2, sort_keys=True), encoding="utf-8")
    return base / name

m = build("prior")
res = {"window": rw.WINDOW_ID, "seal": m["seal"]["exclusive_end"],
       "sealed": {x: m["source_checks"][x].get("rows_sealed_dropped") for x in fields},
       "legacy_key_published": "rows_available_on_or_after_2025_dropped" in json.dumps(m)}
res["same_seal_reused"] = build("same", reuse=base / "prior")["reuse"]["reused"]
try:
    build("other", reuse=edited("prior-2025", lambda d: d["seal"].update(exclusive_end="2025-01-01")))
    res["refused"] = None
except rw.SealError as err:
    res["refused"] = str(err)
res["other_published"] = (base / "other" / "manifest.json").exists()
res["other_payloads"] = sorted(p.name for p in (base / "other").glob("*.f64"))
log = io.StringIO()
with contextlib.redirect_stderr(log):
    legacy = build("legacy", reuse=edited("prior-no-seal", lambda d: d.pop("seal")))
res["legacy_reused"] = legacy["reuse"]["reused"]
res["legacy_logged"] = "no seal.exclusive_end recorded" in log.getvalue()
print(json.dumps(res))
"""


class SealUnderTheRepositoryWindow(unittest.TestCase):
    def test_load_prior_refuses_another_seal_and_the_sealed_key_is_published(self):
        with tempfile.TemporaryDirectory() as temp:
            got = isolated(TOOLS, CODE, temp, FIXTURE)
        self.assertEqual((got["window"], got["seal"]), ("research-window-v2", "2024-01-01"))
        self.assertEqual(got["sealed"], {"si_shares": 1, "si_dtc": 0})   # the 2024-02-07 probe row
        self.assertFalse(got["legacy_key_published"])
        self.assertEqual(got["same_seal_reused"], ["si_shares", "si_dtc"])
        self.assertIn("built under the research seal 2025-01-01, not 2024-01-01 (research-window-v2)", got["refused"])
        self.assertEqual((got["other_published"], got["other_payloads"]), (False, []))   # refused before any copy
        self.assertEqual(got["legacy_reused"], ["si_shares", "si_dtc"])
        self.assertTrue(got["legacy_logged"])


if __name__ == "__main__":
    unittest.main()
