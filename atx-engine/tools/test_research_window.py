"""research_window.py (platform v8 W0-1): the repository window, its JSON source, the C++ header and the tools'
refusals under it. Synthetic roles only.

conftest.py binds the superseded window for the field-builder fixtures of this directory, so every check of the
repository window as a tool sees it runs in a fresh interpreter (``isolated``) that never loads conftest.py.
"""
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

import numpy as np

import research_window as rw

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[1]
IMPL_TOOLS = REPO / "atx-impl" / "tools"
HEADER = REPO / "atx-engine" / "include" / "atx" / "engine" / "data" / "research_window.hpp"
DAY_NS = 86_400_000_000_000
EPOCH = dt.date(1970, 1, 1)
NAMES = ("WINDOW_ID", "TRAIN_BEGIN_DATE", "TRAIN_END_DATE", "SEAL_DATE", "TRAIN_BEGIN_NS", "TRAIN_END_NS", "SEAL_NS",
         "FIRST_SEALED_YEAR")
MODULE_CODE = ("import json, research_window as rw; "
               f"print(json.dumps(dict({{k: getattr(rw, k) for k in {NAMES!r}}}, SEAL=rw.SEAL.isoformat())))")
# One tool call on a pinned role; prints what it raised (argv: manifest path, manifest SHA-256, u pass directory).
CALL = '''
import argparse, json, sys
from pathlib import Path
manifest, pin, u_pass = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
try:
    {call}
    out = {{"raised": None}}
except Exception as exc:
    out = {{"raised": type(exc).__name__, "value_error": isinstance(exc, ValueError), "message": str(exc)}}
print(json.dumps(out))
'''
TOOL_CALLS = {  # tool -> (directory it runs from, the call)
    "prepare_research_fields": (TOOLS, "import prepare_research_fields as t; t.Role(manifest.parent, pin)"),
    "fit_composition_weights": (IMPL_TOOLS, "import fit_composition_weights as t; t.RoleManifest(manifest, pin)"),
    "alpha_report_card": (IMPL_TOOLS, "import alpha_report_card as t; "
                                      "t.Inputs(argparse.Namespace(u_pass=u_pass, train=manifest, train_sha256=pin))"),
}


def ns(text: str) -> int:
    return (dt.date.fromisoformat(text) - EPOCH).days * DAY_NS


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def isolated(cwd: Path, code: str, *args) -> dict:
    """Run ``code`` in a fresh interpreter from ``cwd`` (no conftest.py) and return the JSON object it prints last."""
    done = subprocess.run([sys.executable, "-c", code, *map(str, args)], cwd=cwd, capture_output=True, text=True,
                          timeout=600, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    if done.returncode != 0:
        raise AssertionError(f"isolated run failed:\n{done.stderr}")
    return json.loads(done.stdout.strip().splitlines()[-1])


def write_role(root: Path, last: dt.date, dates: int = 12) -> tuple[Path, str]:
    """A synthetic pinned atx.recent-research-role/v1: ``dates`` weekday sessions, the last one ``last``."""
    root.mkdir(parents=True)
    days, d = [], last
    while len(days) < dates:
        if d.weekday() < 5:
            days.append(d)
        d -= dt.timedelta(days=1)
    sessions = np.array([ns(x.isoformat()) for x in reversed(days)], dtype="<i8")
    files = {}
    for name, array in (("sessions.i64", sessions), ("ids.u64", np.array([101, 202, 303], dtype="<u8")),
                        ("member.u8", np.ones((dates, 3), dtype="u1"))):
        blob = array.tobytes()
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": sha(blob)}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete",
                "instrument_namespace": "spiderrock.securityID", "dates": dates, "instruments": 3, "score_begin": 2,
                "score_end": dates, "score_start_ns": int(sessions[2]), "score_end_ns": int(sessions[-1]) + DAY_NS,
                "files": files}
    blob = json.dumps(manifest).encode()
    (root / "manifest.json").write_bytes(blob)
    return root / "manifest.json", sha(blob)


def run_tools(manifest: Path, pin: str, u_pass: Path) -> dict:
    return {tool: isolated(cwd, CALL.format(call=call), manifest, pin, u_pass)
            for tool, (cwd, call) in TOOL_CALLS.items()}


class ResearchWindow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = json.loads(rw.PATH.read_text(encoding="utf-8"))
        cls.module = isolated(TOOLS, MODULE_CODE)   # the repository window, as every tool binds it

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.u_pass = self.root / "u"
        self.u_pass.mkdir()
        for name in ("summary.json", "orientations.json"):
            (self.u_pass / name).write_text("{}", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def test_window_is_the_owner_ruling(self):
        doc = self.doc
        self.assertEqual(doc["schema"], "atx.research-window/v2")
        self.assertEqual((doc["train_begin"], doc["train_end_exclusive"], doc["seal_begin"]),
                         ("2020-01-01", "2024-01-01", "2024-01-01"))
        self.assertEqual(doc["supersedes"]["schema"], "research-seal-v1")
        self.assertEqual(doc["owner_ruling"]["date"], "2026-09-29")
        self.assertEqual(self.module["WINDOW_ID"], "research-window-v2")
        self.assertTrue(rw.PATH.read_bytes().endswith(b"}\n") and b"\r\n" not in rw.PATH.read_bytes())

    def test_json_and_module_agree(self):
        doc = self.doc
        expected = {"WINDOW_ID": rw.window_id(doc["schema"]), "TRAIN_BEGIN_DATE": doc["train_begin"],
                    "TRAIN_END_DATE": doc["train_end_exclusive"], "SEAL_DATE": doc["seal_begin"],
                    "TRAIN_BEGIN_NS": ns(doc["train_begin"]), "TRAIN_END_NS": ns(doc["train_end_exclusive"]),
                    "SEAL_NS": ns(doc["seal_begin"]), "FIRST_SEALED_YEAR": int(doc["seal_begin"][:4]),
                    "SEAL": doc["seal_begin"]}
        self.assertEqual(self.module, expected)
        current = rw.current()
        self.assertEqual({k: current[k] for k in NAMES}, {k: expected[k] for k in NAMES})
        self.assertEqual(current["SEAL"].isoformat(), expected["SEAL"])
        old = rw.superseded()   # the window conftest.py binds for the fixtures: research-seal-v1 as recorded
        self.assertEqual((old["WINDOW_ID"], old["TRAIN_END_DATE"], old["SEAL_DATE"]),
                         (doc["supersedes"]["schema"], doc["supersedes"]["train_end_exclusive"],
                          doc["supersedes"]["seal_begin"]))

    def test_header_matches_json(self):
        text = HEADER.read_text(encoding="utf-8")

        def constant(name):
            m = re.search(rf"\b{name} = ([0-9']+)LL;", text)
            self.assertIsNotNone(m, name)
            return int(m[1].replace("'", ""))

        def literal(name):
            m = re.search(rf'\b{name} = "([^"]*)";', text)
            self.assertIsNotNone(m, name)
            return m[1]

        self.assertEqual(constant("kTrainBeginNs"), ns(self.doc["train_begin"]))
        self.assertEqual(constant("kTrainEndExclusiveNs"), ns(self.doc["train_end_exclusive"]))
        self.assertEqual(constant("kSealBeginNs"), ns(self.doc["seal_begin"]))
        self.assertEqual(literal("kResearchWindowId"), self.module["WINDOW_ID"])
        self.assertEqual(literal("kSealBeginDate"), self.doc["seal_begin"])

    def test_seal_refuses_2024(self):
        seal = dt.date.fromisoformat(self.module["SEAL_DATE"])
        last = seal + dt.timedelta(days=1)                       # 2024-01-02 under research-window-v2
        self.assertLess(last.weekday(), 5)
        manifest, pin = write_role(self.root / "sealed", last)
        for tool, got in run_tools(manifest, pin, self.u_pass).items():
            with self.subTest(tool):
                self.assertIsNotNone(got["raised"], tool)
                self.assertTrue(got["value_error"], got)
                self.assertIn(self.module["WINDOW_ID"], got["message"])
                self.assertIn(self.module["SEAL_DATE"], got["message"])

    def test_2023_session_is_train(self):
        train_end = dt.date.fromisoformat(self.module["TRAIN_END_DATE"])
        last = train_end - dt.timedelta(days=1)
        while last.weekday() >= 5:
            last -= dt.timedelta(days=1)                         # 2023-12-29, the last TRAIN weekday
        self.assertEqual(last.year, train_end.year - 1)
        last_ns = ns(last.isoformat())
        self.assertTrue(self.module["TRAIN_BEGIN_NS"] <= last_ns < self.module["TRAIN_END_NS"])
        self.assertLess(last_ns, self.module["SEAL_NS"])
        manifest, pin = write_role(self.root / "train", last)
        got = run_tools(manifest, pin, self.u_pass)
        self.assertIsNone(got["prepare_research_fields"]["raised"], got["prepare_research_fields"])
        self.assertIsNone(got["fit_composition_weights"]["raised"], got["fit_composition_weights"])
        # the card admits the role, then refuses this empty u pass (it is not a u pass): not a seal refusal
        card = got["alpha_report_card"]
        self.assertEqual(card["raised"], "CardError", card)
        self.assertIn("u pass", card["message"])
        self.assertNotIn(self.module["WINDOW_ID"], card["message"])

    def test_helpers(self):
        self.assertEqual(rw.window_id("atx.research-window/v2"), "research-window-v2")
        with self.assertRaises(ValueError):
            rw.window_values("x", "2020-01-01", "2024-01-01", "2023-06-01")   # the seal before the TRAIN end
        with self.assertRaises(ValueError):
            rw.window_values("x", "2020-01-01", "2024-1-1", "2024-01-01")     # not YYYY-MM-DD
        with self.assertRaises(ValueError):
            rw.bind({"WINDOW_ID": "partial"})
        self.assertTrue(issubclass(rw.SealError, ValueError))


if __name__ == "__main__":
    unittest.main()
