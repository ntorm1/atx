#!/usr/bin/env python3
"""The fitter's ``--walk-forward`` post-step (P9 lane D3W, contract K-P9-8, rulings D3-1 / D3-2;
composition_engine_fit.run_post_step / weights_v3) on the ``--engine-fit`` stand-in world of
test_composition_engine_fit.

The C++ executables are that file's Python stand-ins; under ``--walk-forward`` the fit stand-in also writes the
walk_forward block ``atx-research-composition fit --walk-forward`` writes (each row the Python composition on its
window, the TRAIN decisions d <= j - lag: the rule functions the C++ fit is held to by ruling P12). What is tested: the
Python side: one executable call, the v3 document (the engine document plus the rows, every library id), main()'s
routing, flag absent = main() is fit() byte for byte, and every refusal publishes nothing and leaves no stage. The C++
rows themselves are the gtests' (WalkForward.*).
"""
from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import composition_engine_fit as cef  # noqa: E402
import composition_ic_shrink as cis  # noqa: E402
import composition_mom_volman as cmv  # noqa: E402
import composition_rules as cr  # noqa: E402
import composition_theme_erc as cte  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_composition_engine_fit as tcef  # noqa: E402  (its stand-ins and world, not its tests)
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)

LAG, STEP, LEAST = 2, 100, 50  # small enough for the synthetic world's 550 decisions (registered: 2, 21, 21)
WF = dict(walk_forward="expanding", walk_forward_lag=LAG, walk_forward_step=STEP, walk_forward_min_decisions=LEAST)
WF_ARGV = ["--walk-forward", "expanding", "--walk-forward-lag", str(LAG), "--walk-forward-step", str(STEP),
           "--walk-forward-min-decisions", str(LEAST)]
# std-erc and std: every window fits; shrink: the member cap refuses an early window (so the C++ fit refuses the run).
CASES = {"std-erc": dict(tcef.STD_ARGS, theme_erc=cte.RULE_ID), "std": dict(tcef.STD_ARGS),
         "shrink": dict(tcef.STD_ARGS, composition=cis.RULE_ID)}
FITTING = ("std-erc", "std")
CASE_ARGV = {"std-erc": ["--orientation", "prior", "--composition", "ew-theme-std-v1", "--theme-erc", cte.RULE_ID]}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def hooks(limit: int = fcw.METADATA_LIMIT) -> cef.PostStepHooks:
    """What the fitter's main() passes (test MainRoutes runs main() itself)."""
    return cef.PostStepHooks(error=fcw.FitError, canonical=fcw.canonical_bytes, publish=fcw.publish_directory,
                                pending=fcw.pending_path, limit=limit, weights_file=fcw.OUTPUT_WEIGHTS,
                                exit_ok=fcw.EXIT_OK)


def stage_of(out: Path) -> Path:
    return out.with_name(out.name + cef.POST_STAGE)


def python_rows(argv: list) -> dict:
    """``fit --walk-forward expanding --lag N --step N --min-decisions N``'s block on the stand-in: rows at j = first,
    first + step, ... (first: the smallest j whose window, the TRAIN decisions d <= j - lag, holds min-decisions), each
    the Python composition on its window, in the spec's member order."""
    lag, step, least = (int(argv[argv.index(name) + 1]) for name in ("--lag", "--step", "--min-decisions"))
    spec, factors = json.loads(tcef.flag(argv, "--spec").read_bytes()), tcef.flag(argv, "--factors")
    sessions = tcef.read_factor_dir(factors)[3]
    mask, at = tcef.train_mask(sessions), np.arange(len(sessions))
    first = next(j for j in range(lag, len(sessions)) if int(np.sum(mask & (at + lag <= j))) >= least)
    rows = []
    for j in range(first, len(sessions), step):
        window = mask & (at + lag <= j)
        with unittest.mock.patch.object(tcef, "train_mask", lambda s, w=window: w):
            doc = tcef.python_fit_document(spec, factors)
        last = int(np.flatnonzero(window)[-1])
        rows.append({"decision_index": j, "from_session": int(sessions[j]), "fit_decisions": int(window.sum()),
                     "last_fit_index": last, "last_fit_session": int(sessions[last]), "weights": doc["weights"]})
    return {"rule": cef.WALK_FORWARD_RULE, "window": "expanding", "lag": lag, "step": step, "min_decisions": least,
            "rows": rows}


class WalkForwardStandIns(tcef.StandIns):
    """test_composition_engine_fit.StandIns whose fit stand-in also answers --walk-forward (``wf_mutate(block)`` may
    edit the block); ``calls`` records "fit-wf" for that call."""

    def __init__(self, factors: Path, wf_mutate=None, **mutations):
        super().__init__(factors, **mutations)
        self.wf_mutate = wf_mutate

    def run_exe(self, argv, error):
        argv = [str(a) for a in argv]
        if argv[1] != "fit" or "--walk-forward" not in argv:
            return super().run_exe(argv, error)
        self.calls.append("fit-wf")
        try:
            block = python_rows(argv)
        except cef.EngineFitError as exc:  # the executable refuses a row: exit 1, nothing written (run_exe's refusal)
            raise error(f"--engine-fit: {Path(argv[0]).name} fit exited 1: {exc}") from exc
        if self.wf_mutate is not None:
            self.wf_mutate(block)

        def add(doc):
            if self.fit_mutate is not None:
                self.fit_mutate(doc)
            doc["walk_forward"] = block
        tcef.python_fit(argv, add)
        return None


def without_walk_forward(raw: bytes, schema: str) -> dict:
    """The v3 weights document minus what only --walk-forward writes (each must be there): the walk_forward block,
    provenance.walk_forward and provenance.engine_fit (its composition_fit_sha256 hashes the document with the rows);
    the schema back to the flag-absent one."""
    doc = json.loads(raw)
    assert doc["schema"] == cef.WEIGHTS_SCHEMA_V3, doc["schema"]
    del doc["walk_forward"], doc["provenance"]["walk_forward"], doc["provenance"][cef.PROVENANCE_KEY]
    doc["schema"] = schema
    return doc


class World:
    """One synthetic world for the class (registry absent: the declared tier scores)."""

    @staticmethod
    def open(test_class) -> None:
        test_class.tmp = tempfile.TemporaryDirectory()
        test_class.root = Path(test_class.tmp.name)
        test_class.fx = tcef.World.build(test_class.root)
        registry = unittest.mock.patch.object(cr, "REGISTRY_PATH", test_class.root / "no-registry.json")
        registry.start()
        test_class.addClassCleanup(registry.stop)
        test_class.addClassCleanup(test_class.tmp.cleanup)


def run_wf(fx, root: Path, name: str, tag: str, limit: int = fcw.METADATA_LIMIT, **stand) -> tuple[Path, object, list]:
    """One --engine-fit --walk-forward run of CASES[name] through run_post_step: (output dir, (code, summary) or
    the FitError, the stand-ins' calls)."""
    out = root / f"{name}-{tag}"
    with WalkForwardStandIns(root / f"{name}-{tag}-factors", **stand) as stand_ins:
        args = fx.args(out, **CASES[name], **WF, **tcef.placeholders(root / f"{name}-{tag}-in"))
        try:
            got = cef.run_post_step(args, fcw.fit, hooks(limit))
        except fcw.FitError as exc:
            got = exc
    return out, got, stand_ins.calls


class WalkForwardEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        World.open(cls)
        cls.runs = {}
        for name, case in ((n, CASES[n]) for n in FITTING):
            with tcef.StandIns(cls.root / f"{name}-engine-factors"):
                engine_code, engine_summary = fcw.fit(
                    cls.fx.args(cls.root / f"{name}-engine", **case, **tcef.placeholders(cls.root / f"{name}-ein")))
            out, got, calls = run_wf(cls.fx, cls.root, name, "wf")
            cls.runs[name] = dict(engine_code=engine_code, engine_summary=engine_summary, out=out, got=got,
                                  calls=calls, engine=tcef.load(cls.root / f"{name}-engine"), wf=tcef.load(out))

    def test_one_executable_call_writes_the_rows(self):
        for name, run in self.runs.items():
            with self.subTest(name):
                self.assertNotIsInstance(run["got"], Exception, run["got"])
                self.assertEqual((run["engine_code"], run["got"][0]), (fcw.EXIT_OK, fcw.EXIT_OK))
                self.assertEqual(run["calls"], ["screen", "fit-wf"])

    def test_v3_is_the_engine_document_plus_the_rows(self):
        """Admission files identical; the weights file is the --engine-fit one (schema v2) plus the walk_forward block
        and provenance.walk_forward, with engine_fit differing only in composition_fit_sha256."""
        for name, run in self.runs.items():
            with self.subTest(name):
                self.assertEqual(sorted(run["wf"]), sorted(run["engine"]))
                for file in (fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV):
                    self.assertEqual(run["wf"][file], run["engine"][file], file)
                engine = json.loads(run["engine"][fcw.OUTPUT_WEIGHTS])
                stripped = without_walk_forward(run["wf"][fcw.OUTPUT_WEIGHTS], engine["schema"])
                engine_record = engine["provenance"].pop(cef.PROVENANCE_KEY)
                self.assertEqual(stripped, engine)
                record = json.loads(run["wf"][fcw.OUTPUT_WEIGHTS])["provenance"][cef.PROVENANCE_KEY]
                self.assertNotEqual(record["composition_fit_sha256"], engine_record["composition_fit_sha256"])
                record["composition_fit_sha256"] = engine_record["composition_fit_sha256"]
                self.assertEqual(record, engine_record)

    def test_the_block_holds_every_library_id_per_row(self):
        for name, run in self.runs.items():
            with self.subTest(name):
                doc = json.loads(run["wf"][fcw.OUTPUT_WEIGHTS])
                block, members = doc["walk_forward"], doc["provenance"]["candidates"]
                library = [row["id"] for row in members]
                self.assertEqual(sorted(block), ["lag", "min_decisions", "rows", "rule", "step", "window"])
                self.assertEqual([block[k] for k in ("rule", "window", "lag", "step", "min_decisions")],
                                 [cef.WALK_FORWARD_RULE, "expanding", LAG, STEP, LEAST])
                self.assertGreaterEqual(len(block["rows"]), 3)
                prov = doc["provenance"]["walk_forward"]
                self.assertEqual(len(prov["rows"]), len(block["rows"]))
                sessions = [row["from_session"] for row in block["rows"]]
                self.assertEqual(sessions, sorted(set(sessions)))
                fitted = {cid for cid, w in doc["weights"].items() if w > 0}
                for row, detail in zip(block["rows"], prov["rows"]):
                    self.assertEqual(sorted(row), ["from_session", "weights"])
                    self.assertEqual(list(row["weights"]), sorted(library))  # canonical bytes sort the keys
                    self.assertEqual(row["from_session"], detail["from_session"])
                    self.assertLessEqual(detail["last_fit_index"] + LAG, detail["decision_index"])
                    self.assertTrue(all(w >= 0.0 for w in row["weights"].values()))
                    self.assertTrue({cid for cid, w in row["weights"].items() if w > 0} <= fitted)
                if name == "std":  # ew-theme-std-v1 alone weights by tier: every window's fit is the full fit's
                    self.assertTrue(all(row["weights"] == doc["weights"] for row in block["rows"]))
                else:  # theme-erc-v1 reads the window's covariance: the rows move
                    self.assertNotEqual(block["rows"][0]["weights"], block["rows"][-1]["weights"])
                self.assertEqual(prov["composition_fit_sha256"],
                                 run["got"][1]["engine_fit"]["composition_fit_sha256"])

    def test_summary_names_the_published_output_and_no_stage_is_left(self):
        for name, run in self.runs.items():
            with self.subTest(name):
                summary, out = run["got"][1], run["out"]
                self.assertEqual(summary["output"], str(out))
                self.assertEqual(summary["files"], {n: sha(b) for n, b in sorted(run["wf"].items())})
                self.assertEqual(summary["weights_sha256"], sha(run["wf"][fcw.OUTPUT_WEIGHTS]))
                rows = json.loads(run["wf"][fcw.OUTPUT_WEIGHTS])["walk_forward"]["rows"]
                self.assertEqual(summary["walk_forward"], {
                    "rule": cef.WALK_FORWARD_RULE, "rows": len(rows), "first_from_session": rows[0]["from_session"],
                    "last_from_session": rows[-1]["from_session"]})
                self.assertFalse(stage_of(out).exists())
                self.assertFalse(fcw.pending_path(stage_of(out)).exists())
                self.assertFalse(fcw.pending_path(out).exists())


class MainRoutes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        World.open(cls)

    def main(self, argv: list) -> tuple[int, str, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = fcw.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    def engine_argv(self, tag: str) -> list:
        flags = tcef.placeholders(self.root / f"{tag}-in")
        return ["--engine-fit", "--engine-factors", str(flags["engine_factors"]), "--admission-exe",
                str(flags["admission_exe"]), "--composition-exe", str(flags["composition_exe"])]

    def test_walk_forward_goes_through_run_post_step(self):
        out = self.root / "main-wf"
        argv = self.fx.argv(out, "v4-prior-v1", [*CASE_ARGV["std-erc"], *self.engine_argv("main-wf"), *WF_ARGV])
        with WalkForwardStandIns(self.root / "main-wf-factors") as stand:
            code, stdout, stderr = self.main(argv)
        self.assertEqual(code, fcw.EXIT_OK, stderr)
        self.assertEqual(stand.calls, ["screen", "fit-wf"])
        summary = json.loads(stdout)
        self.assertEqual((summary["output"], summary["walk_forward"]["rule"]), (str(out), cef.WALK_FORWARD_RULE))
        self.assertEqual(summary["weights_sha256"], sha((out / fcw.OUTPUT_WEIGHTS).read_bytes()))
        self.assertEqual(json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())["schema"], cef.WEIGHTS_SCHEMA_V3)
        self.assertFalse(stage_of(out).exists())
        # The same run through run_post_step directly writes the same bytes (main adds nothing).
        direct, got, _ = run_wf(self.fx, self.root, "std-erc", "direct")
        self.assertNotIsInstance(got, Exception, got)
        self.assertEqual(tcef.load(out), tcef.load(direct))

    def test_flag_absent_main_is_fit_byte_for_byte(self):
        """Without --walk-forward main() never reaches run_post_step and writes what fit() writes."""
        for tag, extra in (("plain", []), ("engine", self.engine_argv("absent-engine"))):
            with self.subTest(tag):
                out_main, out_fit = self.root / f"absent-{tag}-main", self.root / f"absent-{tag}-fit"
                argv = [*CASE_ARGV["std-erc"], *extra]
                refuse = unittest.mock.patch.object(cef, "run_post_step", side_effect=AssertionError("routed"))
                with tcef.StandIns(self.root / f"absent-{tag}-main-factors"), refuse:
                    code, stdout, stderr = self.main(self.fx.argv(out_main, "v4-prior-v1", argv))
                self.assertEqual(code, fcw.EXIT_OK, stderr)
                with tcef.StandIns(self.root / f"absent-{tag}-fit-factors"):
                    fit_code, fit_summary = fcw.fit(fcw.parse_args(self.fx.argv(out_fit, "v4-prior-v1", argv)))
                self.assertEqual(fit_code, fcw.EXIT_OK)
                self.assertEqual(tcef.load(out_main), tcef.load(out_fit))
                summary = json.loads(stdout)
                self.assertNotIn("walk_forward", summary)
                self.assertEqual(summary["weights_sha256"], fit_summary["weights_sha256"])
                doc = json.loads((out_main / fcw.OUTPUT_WEIGHTS).read_bytes())
                self.assertNotIn("walk_forward", doc)
                self.assertNotIn("walk_forward", doc["provenance"])
                self.assertNotEqual(doc["schema"], cef.WEIGHTS_SCHEMA_V3)


class Refusals(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        World.open(cls)

    def ns(self, **kw) -> argparse.Namespace:
        base = dict(engine_fit=True, theme_tsmom=None, theme_resid=None, walk_forward=None, walk_forward_lag=None,
                    walk_forward_step=None, walk_forward_min_decisions=None)
        base.update(kw)
        return argparse.Namespace(**base)

    def test_check_walk_forward_before_any_compute(self):
        armed = {cef.POST_ARMED: "stage"}
        wf = dict(walk_forward="expanding", walk_forward_lag=2, walk_forward_step=21)
        cases = [
            (dict(walk_forward_lag=2), False, "read only with --walk-forward"),
            (dict(walk_forward_min_decisions=21), False, "read only with --walk-forward"),
            (dict(wf, engine_fit=False, **armed), False, "runs under --engine-fit"),
            (dict(wf, theme_tsmom="theme-tsmom-v1", **armed), False, "refused with --theme-tsmom"),
            (dict(wf, theme_resid="theme-resid-v1", **armed), False, "refused with --theme-tsmom"),
            (dict(wf, **armed), True, "refused with --theme-tsmom"),  # pooled (--era)
            (dict(walk_forward="expanding", walk_forward_lag=2, **armed), False, "needs --walk-forward-lag and"),
            (dict(walk_forward="expanding", walk_forward_step=21, **armed), False, "needs --walk-forward-lag and"),
            (dict(wf, walk_forward_lag=1, **armed), False, "lag >= 2"),
            (dict(wf, walk_forward_step=0, **armed), False, "lag >= 2"),
            (dict(wf, walk_forward_min_decisions=1, **armed), False, "lag >= 2"),
        ]
        for kw, pooled, needle in cases:
            with self.subTest(kw=kw, pooled=pooled):
                with self.assertRaises(fcw.FitError) as caught:
                    cef.check_walk_forward(self.ns(**kw), pooled, fcw.FitError)
                self.assertIn(needle, str(caught.exception))
        cef.check_walk_forward(self.ns(), False, fcw.FitError)  # flag absent: nothing to refuse
        cef.check_walk_forward(self.ns(**wf, **armed), False, fcw.FitError)
        # fit() alone (run_post_step did not arm it) under --walk-forward or --engine-fit --mom-volman: POST_ONLY.
        for kw in (wf, dict(mom_volman="mom-volman-v1")):
            with self.subTest(fit_alone=kw), self.assertRaises(fcw.FitError) as caught:
                cef.check_args(self.ns(**kw), True, False, fcw.FitError)
            self.assertIn("through the fitter's main()", str(caught.exception))
        self.assertFalse(cef.post_step(self.ns(mom_volman="mom-volman-v1", engine_fit=False)))
        self.assertTrue(cef.post_step(self.ns(mom_volman="mom-volman-v1")))
        self.assertTrue(cef.post_step(self.ns(**wf)))
        self.assertFalse(cef.post_step(self.ns()))
        self.assertEqual(cef.walk_forward_options(self.ns(**wf)), (2, 21, cef.WALK_FORWARD_MIN_DECISIONS))
        self.assertEqual(cef.WALK_FORWARD_MIN_DECISIONS, 21)  # ruling D3-1

    def test_fit_alone_is_refused_and_publishes_nothing(self):
        out = self.root / "fit-alone"
        with WalkForwardStandIns(self.root / "fit-alone-factors") as stand:
            args = self.fx.args(out, **CASES["std-erc"], **WF, **tcef.placeholders(self.root / "fit-alone-in"))
            with self.assertRaises(fcw.FitError) as caught:
                fcw.fit(args)
        self.assertIn("through the fitter's main()", str(caught.exception))
        self.assertEqual(stand.calls, [])
        self.assertFalse(out.exists())

    def test_an_existing_output_or_a_stale_stage_is_refused_before_the_fit(self):
        for tag, make, needle in (("exists", lambda out: out.mkdir(), "output exists"),
                                  ("stage", lambda out: stage_of(out).mkdir(), "stale or concurrent"),
                                  ("pending", lambda out: fcw.pending_path(stage_of(out)).mkdir(),
                                   "stale or concurrent")):
            with self.subTest(tag):
                out = self.root / f"refuse-{tag}"
                make(out)
                args = self.fx.args(out, **CASES["std-erc"], **WF, **tcef.placeholders(self.root / f"r-{tag}-in"))
                before = sorted(p.name for p in self.root.iterdir())
                fit = unittest.mock.Mock(side_effect=AssertionError("the fit ran"))
                with self.assertRaises(fcw.FitError) as caught:
                    cef.run_post_step(args, fit, hooks())
                self.assertIn(needle, str(caught.exception))
                fit.assert_not_called()
                self.assertEqual(sorted(p.name for p in self.root.iterdir()), before)

    def assert_refused(self, tag: str, needle: str, **stand) -> None:
        out, got, calls = run_wf(self.fx, self.root, "std-erc", tag, **stand)
        self.assertIsInstance(got, fcw.FitError, tag)
        self.assertIn(needle, str(got), tag)
        self.assertFalse(out.exists(), f"{tag}: nothing is published on a refusal")
        self.assertFalse(stage_of(out).exists(), f"{tag}: no stage is left")
        self.assertFalse(fcw.pending_path(stage_of(out)).exists(), f"{tag}: no pending stage is left")

    def test_a_malformed_block_is_refused(self):
        def row_edit(edit):
            def mutate(block):
                edit(block["rows"][1])
            return mutate
        cases = {
            "lag": (lambda b: b.update(lag=3), "carries no walk-forward-v1 block"),
            "rule": (lambda b: b.update(rule="walk-forward-v2"), "carries no walk-forward-v1 block"),
            "window": (lambda b: b.update(window="rolling"), "carries no walk-forward-v1 block"),
            "no-rows": (lambda b: b.update(rows=[]), "carries no walk-forward-v1 block"),
            "negative": (row_edit(lambda r: r["weights"].__setitem__(0, -0.25)), "row 1 is malformed"),
            "nan": (row_edit(lambda r: r["weights"].__setitem__(0, float("nan"))), "row 1 is malformed"),
            "short": (row_edit(lambda r: r["weights"].pop()), "row 1 is malformed"),
            "nothing": (row_edit(lambda r: r.update(weights=[0.0] * len(r["weights"]))), "row 1 is malformed"),
            "order": (row_edit(lambda r: r.update(decision_index=0)), "row 1 is malformed"),
            "key": (row_edit(lambda r: r.pop("last_fit_session")), "row 1 is malformed"),
            "bool": (row_edit(lambda r: r.update(fit_decisions=True)), "row 1 is malformed"),
        }
        for tag, (mutate, needle) in cases.items():
            with self.subTest(tag):
                self.assert_refused(f"block-{tag}", needle, wf_mutate=mutate)

    def test_the_metadata_bound_is_refused(self):
        out, got, _ = run_wf(self.fx, self.root, "std-erc", "bound", limit=1000)
        self.assertIsInstance(got, fcw.FitError)
        self.assertIn("exceeds the runner's 1000-byte bound", str(got))
        self.assertFalse(out.exists())
        self.assertFalse(stage_of(out).exists())

    def test_a_cpp_disagreement_still_refuses_first(self):
        """--engine-fit's own checks run before the rows are read (a nudged weight beyond ruling P12)."""
        self.assert_refused("disagree", "the C++ fit differs", fit_mutate=tcef.nudge_floats(1.0 + 1e-9))

    def test_a_row_the_fit_refuses_refuses_the_run(self):
        """ic-shrink-v1's member cap refuses an early window of this world: the executable exits 1 and nothing is
        published (no row is skipped, ruling D3-1: every row is a fit or the run refuses)."""
        out, got, calls = run_wf(self.fx, self.root, "shrink", "row-refused")
        self.assertIsInstance(got, fcw.FitError)
        self.assertIn("fit exited 1", str(got))
        self.assertIn("member cap", str(got))
        self.assertEqual(calls, ["screen", "fit-wf"])
        self.assertFalse(out.exists())
        self.assertFalse(stage_of(out).exists())


# ---------------------------------------------------------------- --engine-fit --mom-volman (ruling ALCOMB-7)
VOLMAN = dict(tcef.STD_ARGS, theme_erc=cte.RULE_ID, mom_volman=cmv.RULE_ID)
VOLMAN_ARGV = ["--orientation", "prior", "--composition", "ew-theme-std-v1", "--theme-erc", cte.RULE_ID,
               "--mom-volman", cmv.RULE_ID]


def volman_world(root: Path):
    """test_composition_mom_volman's world: medium and medium_half weighted price_momentum members."""
    panel, signals, ids, extra = tfw.aim_world()
    extra = dict(extra)
    extra["flip"] = dict(extra["flip"], theme="value")
    extra["medium_half"] = dict(extra["medium_half"], tier="C+", theme="price_momentum")
    extra["medium"] = dict(extra["medium"], theme="price_momentum")
    return tfw.Fixture(root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                       candidate_extra=extra)


def scale_sleeve(scale: float):
    """A fit stand-in mutation: the C++ document's mom_volman sleeve times ``scale``."""
    def mutate(doc):
        doc["mom_volman"]["sleeve"] = [x * scale for x in doc["mom_volman"]["sleeve"]]
    return mutate


class MomVolmanEngineFit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.fx = volman_world(cls.root)
        registry = unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json")
        registry.start()
        cls.addClassCleanup(registry.stop)
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.plain_code, _ = fcw.fit(cls.fx.args(cls.root / "plain", **VOLMAN))
        cls.plain = tcef.load(cls.root / "plain")

    def run_volman(self, tag: str, extra: dict | None = None, **stand) -> tuple[Path, object, list]:
        out = self.root / tag
        with WalkForwardStandIns(self.root / f"{tag}-factors", **stand) as stand_ins:
            args = self.fx.args(out, **VOLMAN, **(extra or {}), **tcef.placeholders(self.root / f"{tag}-in"))
            try:
                got = cef.run_post_step(args, fcw.fit, hooks())
            except fcw.FitError as exc:
                got = exc
        return out, got, stand_ins.calls

    def test_the_cpp_sleeve_is_checked_and_written(self):
        """Stand-ins equal to the Python: the document is the flag-absent one plus provenance.engine_fit and
        provenance.mom_volman.sleeve_source; check 3 covered the mom_volman block."""
        self.assertEqual(self.plain_code, fcw.EXIT_OK)
        out, got, calls = self.run_volman("agree")
        self.assertNotIsInstance(got, Exception, got)
        self.assertEqual(got[0], fcw.EXIT_OK)
        self.assertEqual(calls, ["screen", "fit"])
        self.assertIn("mom_volman", got[1]["engine_fit"]["blocks"])
        self.assertIn("mom_volman", got[1]["engine_fit"]["fit_max_pure_rel_diff_by_block"])
        written = tcef.load(out)
        doc = json.loads(written[fcw.OUTPUT_WEIGHTS])
        self.assertEqual(doc["provenance"]["mom_volman"].pop("sleeve_source"), cef.VOLMAN_SOURCE)
        del doc["provenance"][cef.PROVENANCE_KEY]
        self.assertEqual(fcw.canonical_bytes(doc), self.plain[fcw.OUTPUT_WEIGHTS])
        self.assertEqual(got[1]["weights_sha256"], sha(written[fcw.OUTPUT_WEIGHTS]))
        self.assertFalse(stage_of(out).exists())

    def test_a_cpp_sleeve_within_p12_is_the_sleeve_written(self):
        out, got, _ = self.run_volman("within", fit_mutate=scale_sleeve(1.0 + 2.0 ** -50))
        self.assertNotIsInstance(got, Exception, got)
        written = json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())["theme_schedule"]["sleeve"]
        python = json.loads(self.plain[fcw.OUTPUT_WEIGHTS])["theme_schedule"]["sleeve"]
        self.assertEqual(written, [x * (1.0 + 2.0 ** -50) for x in python])
        self.assertNotEqual(written, python)

    def test_a_cpp_sleeve_beyond_p12_is_refused(self):
        out, got, _ = self.run_volman("beyond", fit_mutate=scale_sleeve(1.0 + 1e-6))
        self.assertIsInstance(got, fcw.FitError)
        self.assertIn("the C++ fit differs", str(got))
        self.assertIn("mom_volman.sleeve", str(got))
        self.assertFalse(out.exists())
        self.assertFalse(stage_of(out).exists())

    def test_with_walk_forward_both_steps_run(self):
        out, got, calls = self.run_volman("with-wf", extra=WF, fit_mutate=scale_sleeve(1.0 + 2.0 ** -50))
        self.assertNotIsInstance(got, Exception, got)
        self.assertEqual(calls, ["screen", "fit-wf"])
        doc = json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertEqual(doc["schema"], cef.WEIGHTS_SCHEMA_V3)
        self.assertEqual(doc["provenance"]["mom_volman"]["sleeve_source"], cef.VOLMAN_SOURCE)
        python = json.loads(self.plain[fcw.OUTPUT_WEIGHTS])["theme_schedule"]["sleeve"]
        self.assertEqual(doc["theme_schedule"]["sleeve"], [x * (1.0 + 2.0 ** -50) for x in python])
        self.assertGreaterEqual(len(doc["walk_forward"]["rows"]), 3)

    def test_main_routes_engine_fit_mom_volman(self):
        out = self.root / "main"
        flags = tcef.placeholders(self.root / "main-in")
        argv = self.fx.argv(out, "v4-prior-v1", [
            *VOLMAN_ARGV, "--engine-fit", "--engine-factors", str(flags["engine_factors"]), "--admission-exe",
            str(flags["admission_exe"]), "--composition-exe", str(flags["composition_exe"])])
        stdout = io.StringIO()
        with WalkForwardStandIns(self.root / "main-factors"), contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(io.StringIO()):
            code = fcw.main(argv)
        self.assertEqual(code, fcw.EXIT_OK)
        summary = json.loads(stdout.getvalue())
        self.assertEqual(summary["output"], str(out))
        doc = json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertEqual(doc["provenance"]["mom_volman"]["sleeve_source"], cef.VOLMAN_SOURCE)
        self.assertFalse(stage_of(out).exists())

    def test_engine_sleeve_refusals(self):
        doc = json.loads(self.plain[fcw.OUTPUT_WEIGHTS])
        sleeve = doc["theme_schedule"]["sleeve"]
        cases = [
            ({k: v for k, v in doc.items() if k != "theme_schedule"}, sleeve, "carries no mom-volman-v1 sleeve"),
            (doc, sleeve[:-1], "carries no mom-volman-v1 sleeve"),
            (dict(doc, theme_schedule=dict(doc["theme_schedule"], rule="theme-tsmom-v1")), sleeve,
             "carries no mom-volman-v1 sleeve"),
            (doc, [x + 1e-6 for x in sleeve], "the written sleeve differs from the C++ fit's"),
        ]
        for n, (document, cpp, needle) in enumerate(cases):
            with self.subTest(n), self.assertRaises(fcw.FitError) as caught:
                cef.engine_sleeve(document, {"sleeve": cpp}, fcw.FitError)
            self.assertIn(needle, str(caught.exception))
        same = cef.engine_sleeve(doc, {"sleeve": sleeve}, fcw.FitError)
        self.assertEqual(same["theme_schedule"], doc["theme_schedule"])
        self.assertNotIn("sleeve_source", doc["provenance"]["mom_volman"])  # the input is not edited


class WeightsV3(unittest.TestCase):
    """weights_v3 on a small document: every library id per row, a row-weighted zero-weight member's fitted theme
    added to theme_standardise. P9 lane D3W-PB (review D3W S-8): no copy of the IC runner's refusal (a lifted
    candidate's sign and theme) is kept here; the runner refuses such a file before any payload
    (WeightsV3.RefusalsPrecedeAnyPayloadOrOutput)."""

    DOC = {"schema": "atx.dsl-composition-weights/v2", "signs": {"a": 1, "b": 1, "c": 1},
           "weights": {"a": 0.5, "b": 0.5, "c": 0.0, "d": 0.0},
           "theme_standardise": {"rule": "ew-theme-std-v1", "rerank": False, "themes": {"a": "x", "b": "y"}},
           "provenance": {"rule": "ew-theme-std-v1"}}

    def rows(self, weights, themes=("x", "y", "x")) -> dict:
        block = {"rule": cef.WALK_FORWARD_RULE, "window": "expanding", "lag": 2, "step": 21, "min_decisions": 21,
                 "rows": [{"decision_index": 22 + 21 * k, "from_session": 1000 + k, "fit_decisions": 21 + 21 * k,
                           "last_fit_index": 20 + 21 * k, "last_fit_session": 998 + k, "weights": w}
                          for k, w in enumerate(weights)]}
        return {"members": ["a", "b", "c"], "member_themes": list(themes), "block": block,
                "composition_fit_sha256": "ab" * 32}

    def test_rows_and_an_added_theme(self):
        doc = copy.deepcopy(self.DOC)
        got = cef.weights_v3(doc, self.rows([[0.5, 0.5, 0.0], [0.25, 0.25, 0.5]]))
        self.assertEqual(doc, self.DOC)  # the input is not edited
        self.assertEqual(got["schema"], cef.WEIGHTS_SCHEMA_V3)
        self.assertEqual(got["walk_forward"]["rows"], [
            {"from_session": 1000, "weights": {"a": 0.5, "b": 0.5, "c": 0.0, "d": 0.0}},
            {"from_session": 1001, "weights": {"a": 0.25, "b": 0.25, "c": 0.5, "d": 0.0}}])
        self.assertEqual(got["theme_standardise"]["themes"], {"a": "x", "b": "y", "c": "x"})
        self.assertEqual((got["weights"], got["signs"]), (self.DOC["weights"], self.DOC["signs"]))
        self.assertEqual([r["decision_index"] for r in got["provenance"]["walk_forward"]["rows"]], [22, 43])
        self.assertNotIn("weights", got["provenance"]["walk_forward"]["rows"][0])
        plain = cef.weights_v3(self.DOC, self.rows([[0.5, 0.5, 0.0]]))
        self.assertEqual(plain["theme_standardise"]["themes"], {"a": "x", "b": "y"})  # nothing to add

    def test_the_runner_owns_the_lifted_candidate_refusal(self):
        """The documents the runner refuses are written as the C++ rows give them (the theme mapping only)."""
        other = cef.weights_v3(self.DOC, self.rows([[0.5, 0.0, 0.5]], themes=("x", "y", "z")))
        self.assertEqual(other["theme_standardise"]["themes"], {"a": "x", "b": "y", "c": "z"})
        unsigned = dict(copy.deepcopy(self.DOC), signs={"a": 1, "b": 1})
        got = cef.weights_v3(unsigned, self.rows([[0.5, 0.0, 0.5]]))
        self.assertEqual(got["signs"], {"a": 1, "b": 1})
        self.assertEqual(got["walk_forward"]["rows"][0]["weights"], {"a": 0.5, "b": 0.0, "c": 0.5, "d": 0.0})


@unittest.skipUnless(os.environ.get("ATX_RESEARCH_ADMISSION_EXE") and os.environ.get("ATX_RESEARCH_COMPOSITION_EXE"),
                     "needs built atx-research-admission and atx-research-composition")
class RealExecutables(unittest.TestCase):
    """The built executables end to end: --walk-forward's rows are the C++ fit's (decision, session and window counts
    exactly, every weight within ruling P12 of the Python composition on the same window), and --engine-fit
    --mom-volman writes the C++ sleeve, within P12 of the flag-absent fit's."""

    def run_real(self, fx, root: Path, tag: str, case: dict) -> tuple[dict, dict, dict]:
        """(the published weights document, the summary, python_rows on the spec and series the C++ fit read plus
        the spec's member order)."""
        exes = dict(admission_exe=Path(os.environ["ATX_RESEARCH_ADMISSION_EXE"]),
                    composition_exe=Path(os.environ["ATX_RESEARCH_COMPOSITION_EXE"]))
        real_check, real_run, fits = cef.check_factor_series, cef.run_exe, []

        def check(directory, state, error):
            tcef.write_factor_dir(Path(directory), state)
            return real_check(directory, state, error)

        def run(argv, error):
            argv = [str(a) for a in argv]
            if argv[1] == "fit":  # keep the spec the fit read (its work directory is removed after verify)
                kept = root / f"{tag}-spec.json"
                kept.write_bytes(tcef.flag(argv, "--spec").read_bytes())
                fits.append([str(kept) if a == str(tcef.flag(argv, "--spec")) else a for a in argv])
            return real_run(argv, error)
        out = root / tag
        with unittest.mock.patch.object(cef, "check_factor_series", check), \
                unittest.mock.patch.object(cef, "run_exe", run):
            args = fx.args(out, **case, **dict(tcef.placeholders(root / f"{tag}-in"), **exes))
            code, summary = cef.run_post_step(args, fcw.fit, hooks())
        self.assertEqual((code, len(fits)), (fcw.EXIT_OK, 1))
        spec = json.loads(tcef.flag(fits[0], "--spec").read_bytes())
        return json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes()), summary, dict(
            python_rows(fits[0]), members=[m["id"] for m in spec["members"]])

    def assert_rows(self, doc: dict, python: dict) -> None:
        keys = ("decision_index", "from_session", "fit_decisions", "last_fit_index", "last_fit_session")
        cpp = doc["provenance"]["walk_forward"]["rows"]
        self.assertGreaterEqual(len(cpp), 3)
        self.assertEqual([[r[k] for k in keys] for r in cpp], [[r[k] for k in keys] for r in python["rows"]])
        for row, want in zip(doc["walk_forward"]["rows"], python["rows"]):
            self.assertEqual(row["from_session"], want["from_session"])
            self.assertEqual(cef.compare_values(want["weights"], [row["weights"][m] for m in python["members"]])[0],
                             [])
            self.assertEqual([c for c, w in row["weights"].items() if w and c not in python["members"]], [])

    def test_walk_forward_rows_are_the_cpp_fit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fx = tcef.World.build(root)
            with unittest.mock.patch.object(cr, "REGISTRY_PATH", root / "no-registry.json"):
                doc, summary, python = self.run_real(fx, root, "wf", dict(CASES["std-erc"], **WF))
        self.assertEqual(doc["schema"], cef.WEIGHTS_SCHEMA_V3)
        self.assertEqual(summary["walk_forward"]["rows"], len(python["rows"]))
        self.assert_rows(doc, python)

    def test_mom_volman_writes_the_cpp_sleeve_beside_the_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fx = volman_world(root)
            with unittest.mock.patch.object(cr, "REGISTRY_PATH", root / "no-registry.json"):
                self.assertEqual(fcw.fit(fx.args(root / "plain", **VOLMAN))[0], fcw.EXIT_OK)
                plain = json.loads((root / "plain" / fcw.OUTPUT_WEIGHTS).read_bytes())
                doc, summary, python = self.run_real(fx, root, "both", dict(VOLMAN, **WF))
        self.assertIn("mom_volman", summary["engine_fit"]["blocks"])
        self.assertEqual(doc["provenance"]["mom_volman"]["sleeve_source"], cef.VOLMAN_SOURCE)
        self.assertEqual(cef.compare_values(plain["theme_schedule"]["sleeve"], doc["theme_schedule"]["sleeve"])[0],
                         [])
        self.assert_rows(doc, python)


if __name__ == "__main__":
    unittest.main()
