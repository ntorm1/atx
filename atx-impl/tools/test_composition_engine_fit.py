#!/usr/bin/env python3
"""The fitter flag ``--engine-fit`` (P9 lane D2, ruling D2-1; composition_engine_fit.py) on the fitter's synthetic
world (test_fit_composition_weights.aim_world, two themes made scorable as in test_composition_resid).

The C++ executables are replaced by Python stand-ins that read exactly what the executables read: the K-P9-4 factor
directory the fitter's own factor series is written to (``check_factor_series`` is wrapped to write it first, so the
directory is the fitter's series bit for bit unless a case mutates it), the fitter's admission.json as the screen's
candidates file, and the fit spec. The screen stand-in is the fitter's screen_v4 (test_factor_series_admission
.fitter_screen) and the fit stand-in the Python composition functions on the spec (as test_composition_fit_fixture
.run_case does), so on an untouched directory the "C++" values equal the Python's bit for bit and the flag-present
weights file is the flag-absent one plus provenance.engine_fit. Cases then perturb one value inside the stand-ins:
beyond ruling P12's tolerance it is a refusal with nothing published; inside it the perturbed (C++) value is what the
weights file carries.

The real executables run in ``RealExecutables`` when ATX_RESEARCH_ADMISSION_EXE and ATX_RESEARCH_COMPOSITION_EXE name
built atx-research-admission / atx-research-composition (skipped otherwise).
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
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
import composition_resid as cres  # noqa: E402
import composition_rules as cr  # noqa: E402
import composition_theme_erc as cte  # noqa: E402
import composition_theme_tsmom as ct  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_factor_series_admission as tfsa  # noqa: E402  (its screen helpers, not its tests)
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)

STD_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")
CASES = {
    "std-erc-tsmom": dict(STD_ARGS, theme_erc=cte.RULE_ID, theme_tsmom=ct.RULE_ID),
    "shrink-tsmom": dict(STD_ARGS, composition=cis.RULE_ID, theme_tsmom=ct.RULE_ID),
    "std-aim-resid": dict(STD_ARGS, composition=cr.STD_AIM_RULE_ID, theme_resid=cres.RULE_ID),
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------- the factor directory (K-P9-4a's shipped bytes)
def write_factor_dir(directory: Path, state: cef.FitterState, mutate=None) -> None:
    """factor.f64 (decisions x candidates, little-endian), tau.f64, factor_h21.f64 (NaN: the screen's traded-horizon
    table only) and manifest.json of the fitter's own series; ``mutate(f)`` may edit f (decisions x candidates)."""
    directory.mkdir(parents=True, exist_ok=True)
    f = np.ascontiguousarray(np.asarray(state.factors, dtype=np.float64).T)
    if mutate is not None:
        mutate(f)
    payloads = {"factor.f64": f.astype("<f8").tobytes(),
                "tau.f64": np.asarray(state.taus, dtype="<f8").tobytes(),
                "factor_h21.f64": np.full(f.shape, np.nan, dtype="<f8").tobytes()}
    files = {}
    for name, raw in payloads.items():
        (directory / name).write_bytes(raw)
        files[name] = {"bytes": len(raw), "sha256": sha(raw)}
    manifest = {"schema": cef.FACTOR_SCHEMA, "status": "complete", "role_manifest_sha256": state.role_sha256,
                "decisions": int(f.shape[0]), "decision_sessions_ns": [int(s) for s in state.decision_sessions],
                "candidates": [{"id": i} for i in state.library_ids], "files": files}
    (directory / "manifest.json").write_bytes((json.dumps(manifest, indent=2) + "\n").encode())


def read_factor_dir(directory: Path) -> tuple[list[str], np.ndarray, np.ndarray, np.ndarray]:
    """(ids, f decisions x candidates, tau, sessions) as the executables read them."""
    m = json.loads((directory / "manifest.json").read_bytes())
    ids = [c["id"] for c in m["candidates"]]
    f = np.frombuffer((directory / "factor.f64").read_bytes(), dtype="<f8").reshape(m["decisions"], len(ids))
    tau = np.frombuffer((directory / "tau.f64").read_bytes(), dtype="<f8")
    return ids, f, tau, np.asarray(m["decision_sessions_ns"], dtype=np.int64)


def train_mask(sessions: np.ndarray) -> np.ndarray:
    return (sessions >= fcw.FIT_BEGIN_NS) & (sessions < fcw.TRAIN_END_NS)


def flag(argv: list, name: str) -> Path:
    return Path(argv[argv.index(name) + 1])


# ---------------------------------------------------------------- the executables' Python stand-ins
def python_screen(argv: list, mutate=None) -> None:
    """``atx-research-admission screen``: admission.csv and manifest.json of the factor directory."""
    ids, f, tau, sessions = read_factor_dir(flag(argv, "--factors"))
    meta = [dict(c, tau=float(t)) for c, t in zip(json.loads(flag(argv, "--candidates").read_bytes())["candidates"],
                                                   tau)]
    assert [c["id"] for c in meta] == ids
    screen = argv[argv.index("--screen") + 1]
    rows, csv = tfsa.fitter_screen(np.ascontiguousarray(f.T), train_mask(sessions), meta, screen)
    admitted = sorted((k for k, r in enumerate(rows) if r["status"] == "admitted"),
                      key=lambda k: rows[k]["admission_rank"])
    counts: dict = {}
    for r in rows:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    manifest = {"admitted": [ids[k] for k in admitted], "counts": counts,
                "sign_conflicts": [c["id"] for c, r in zip(meta, rows) if c["runner_sign"] != r["s_k"]]}
    if mutate is not None:
        csv, manifest = mutate(csv, manifest)
    out = flag(argv, "--output")
    out.mkdir()
    (out / "admission.csv").write_bytes(csv.encode("utf-8"))
    (out / "manifest.json").write_bytes(json.dumps(manifest).encode())


def python_fit_document(spec: dict, factors_dir: Path) -> dict:
    """``atx-research-composition fit``'s composition_fit.json computed by the Python rule functions on the spec and
    the factor directory (test_composition_fit_fixture.run_case's chain on these inputs)."""
    ids_all, f, _, sessions = read_factor_dir(factors_dir)
    mask = train_mask(sessions)
    members = spec["members"]
    ids, themes = [m["id"] for m in members], [m["theme"] for m in members]
    columns = [ids_all.index(i) for i in ids]
    zero = np.where(np.isnan(f), 0.0, f)
    matrix = np.vstack([m["sign"] * zero[:, c] for m, c in zip(members, columns)])
    gains = [m["gain"] for m in members] if "gain" in members[0] else None
    if spec["composition"] in cr.STD_RULES:
        parent = cr.ew_theme_std(ids, themes, [m["tier"] for m in members], error=cef.EngineFitError, gains=gains)
        prov = parent.provenance
        out = {"member_tiers": [prov["member_tiers"][i] for i in ids],
               "member_scores": [prov["member_scores"][i] for i in ids], "tier_regrades": prov["tier_regrades"]}
    else:
        ics = []
        for c in columns:
            x = f[:, c]
            ics.append(float(x[np.isfinite(x) & mask].mean()))
        parent = cis.ic_shrink(ids, themes, ics, error=cef.EngineFitError, gains=gains)
        prov = parent.provenance
        out = {"ic": ics, "shrunk_ic": [prov["shrunk_ic"][i] for i in ids],
               "within_theme_shares": [prov["within_theme_shares"][i] for i in ids],
               "theme_mean_ic": prov["theme_mean_ic"], "equal_share_themes": prov["equal_share_themes"]}
    out.update(rule=spec["composition"], weights=[float(w) for w in parent.weights],
               weights_before_cap=[prov["weights_before_cap"][i] for i in ids], cap=prov["cap"],
               cap_iterations=prov["cap_iterations"])
    weights = parent.weights
    doc = {"schema": cef.FIT_SCHEMA, "status": "complete", "composition": spec["composition"], "members": ids,
           "member_themes": themes, "train_decisions": int(mask.sum()), "decisions": int(len(sessions)),
           "parent": out}
    if spec.get("theme_erc"):
        erc = cte.theme_erc(parent, ids, themes, matrix, mask, spec["composition"], error=cef.EngineFitError)
        p = erc.provenance
        order = p["theme_order"]
        doc["theme_erc"] = {"within_theme_shares": [p["within_theme_shares"][i] for i in ids], "theme_order": order,
                            "covariance": p["covariance"], "decisions": p["decisions"],
                            "theme_shares": [p["theme_shares"][t] for t in order],
                            "risk_contributions": [p["risk_contributions"][t] for t in order],
                            "dispersion": p["dispersion"], "cap": p["cap"],
                            "weights_before_cap": [p["weights_before_cap"][i] for i in ids],
                            "weights": [float(w) for w in erc.weights], "cap_iterations": p["cap_iterations"]}
        weights = erc.weights
    if spec.get("theme_tsmom"):
        s = ct.schedule(ids, themes, weights, matrix, sessions, mask, "parent", error=cef.EngineFitError)
        b, p = s.block, s.provenance
        doc["theme_tsmom"] = {
            "theme_order": b["themes"], "parent_masses": [p["parent_masses_report_only"][t] for t in b["themes"]],
            "blocks": [{"decision_index": d["decision_index"], "from_session": blk["from_session"],
                        "trailing": blk["trailing"], "masses": [d["masses_report_only"][t] for t in b["themes"]],
                        "themes_off": d["themes_off"]} for blk, d in zip(b["blocks"], p["block_detail"])],
            "theme_blocks_off": p["theme_blocks_off"],
            "theme_on_count": [round(p["theme_on_fraction"][t] * p["blocks"]) for t in b["themes"]]}
    if spec.get("theme_resid"):
        weighted = [t for t, w in zip(themes, weights) if w > 0]
        doc["theme_resid"] = {"order": cres.theme_order(weighted, spec["registered_themes"], cef.EngineFitError)}
    if spec.get("mom_volman"):  # P9 D3W (ruling ALCOMB-7): theme-tsmom-v1's step 1 for price_momentum
        sleeve = ct.sleeves(matrix, themes, [float(w) for w in weights], ["price_momentum"], mask)[0]
        doc["mom_volman"] = {"theme": "price_momentum", "sleeve": [float(x) for x in sleeve]}
    doc["weights"] = [float(w) for w in weights]
    return doc


def python_fit(argv: list, mutate=None) -> None:
    doc = python_fit_document(json.loads(flag(argv, "--spec").read_bytes()), flag(argv, "--factors"))
    if mutate is not None:
        mutate(doc)
    out = flag(argv, "--output")
    out.mkdir()
    (out / cef.FIT_FILE).write_bytes((json.dumps(doc, indent=2) + "\n").encode())


class StandIns:
    """Patches composition_engine_fit for one fitter run: the factor directory written from the fitter's state, the
    screen and fit stand-ins, each with an optional mutation; ``calls`` records the executables' verbs."""

    def __init__(self, factors: Path, factor_mutate=None, screen_mutate=None, fit_mutate=None):
        self.factors, self.calls = factors, []
        self.factor_mutate, self.screen_mutate, self.fit_mutate = factor_mutate, screen_mutate, fit_mutate
        self.real_check = cef.check_factor_series

    def check(self, directory, state, error):
        write_factor_dir(Path(directory), state, self.factor_mutate)
        return self.real_check(directory, state, error)

    def run_exe(self, argv, error):
        argv = [str(a) for a in argv]
        self.calls.append(argv[1])
        if argv[1] == "screen":
            python_screen(argv, self.screen_mutate)
        else:
            assert argv[1] == "fit", argv
            python_fit(argv, self.fit_mutate)

    def __enter__(self):
        self.patches = [unittest.mock.patch.object(cef, "check_factor_series", self.check),
                        unittest.mock.patch.object(cef, "run_exe", self.run_exe)]
        for p in self.patches:
            p.start()
        return self

    def __exit__(self, *exc):
        for p in reversed(self.patches):
            p.stop()


def placeholders(root: Path) -> dict:
    """check_args runs before any compute: a factor directory with a manifest and two executable paths that exist."""
    factors = root / "factors"
    factors.mkdir(parents=True, exist_ok=True)
    (factors / "manifest.json").write_bytes(b"{}\n")
    exes = {}
    for name in ("admission_exe", "composition_exe"):
        exes[name] = root / f"{name}.exe"
        exes[name].write_bytes(b"")
    return dict(engine_fit=True, engine_factors=factors, **exes)


def load(directory: Path) -> dict:
    return {p.name: p.read_bytes() for p in directory.iterdir()}


def without_engine_fit(raw: bytes) -> bytes:
    """The flag-present weights file minus what only the flag writes: provenance.engine_fit and, in an ic-shrink block,
    the ruling D2-S4 IC annotations (ic_source back to the flag-absent text, ic_admission_max_abs_diff removed). Each
    must be there."""
    doc = json.loads(raw)
    del doc["provenance"][cef.PROVENANCE_KEY]
    shrink = doc["provenance"].get("ic_shrink")
    if shrink is not None:
        assert shrink["ic_source"] == cis.IC_SOURCE_ENGINE, shrink["ic_source"]
        assert 0.0 <= shrink.pop("ic_admission_max_abs_diff") <= cef.P12_TOLERANCE
        shrink["ic_source"] = cis.IC_SOURCE
    return fcw.canonical_bytes(doc)


def nudge_floats(scale: float):
    """A fit stand-in mutation: every float of the C++ document's fitted blocks times ``scale`` (C++ rounding inside
    ruling P12 when scale - 1 is far below 1e-12)."""
    def walk(x):
        if isinstance(x, float):
            return x * scale
        if isinstance(x, list):
            return [walk(v) for v in x]
        if isinstance(x, dict):
            return {k: walk(v) for k, v in x.items()}
        return x

    def mutate(doc):
        for key in ("parent", "theme_erc", "theme_tsmom", "weights"):
            if key in doc:
                doc[key] = walk(doc[key])
    return mutate


# ---------------------------------------------------------------- tests
class World:
    """One synthetic world shared by the fitter runs (registry absent: the declared tier scores)."""

    @classmethod
    def build(cls, root: Path):
        panel, signals, ids, extra = tfw.aim_world()
        extra = dict(extra)
        extra["flip"] = dict(extra["flip"], theme="value")           # two themes, five admitted members
        extra["medium_half"] = dict(extra["medium_half"], tier="C+")  # a scored grade
        return tfw.Fixture(root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                           candidate_extra=extra)


class EngineFitEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.fx = World.build(cls.root)
        registry = unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json")
        registry.start()
        cls.addClassCleanup(registry.stop)
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.runs = {}
        for name, case in CASES.items():
            plain_code, plain_summary = fcw.fit(cls.fx.args(cls.root / f"{name}-plain", **case))
            with StandIns(cls.root / f"{name}-factors") as stand:
                args = cls.fx.args(cls.root / f"{name}-engine", **case, **placeholders(cls.root / f"{name}-in"))
                code, summary = fcw.fit(args)
            cls.runs[name] = dict(plain_code=plain_code, plain=load(cls.root / f"{name}-plain"), code=code,
                                  summary=summary, engine=load(cls.root / f"{name}-engine"), calls=stand.calls,
                                  plain_summary=plain_summary)

    def test_agreeing_engine_writes_the_flag_absent_bytes_plus_its_record(self):
        """Stand-ins equal to the Python bit for bit: admission files identical, the weights file identical apart from
        provenance.engine_fit, the summary records the agreement."""
        for name, run in self.runs.items():
            with self.subTest(name):
                self.assertEqual((run["plain_code"], run["code"]), (fcw.EXIT_OK, fcw.EXIT_OK))
                self.assertEqual(run["calls"], ["screen", "fit"])
                self.assertEqual(sorted(run["engine"]), sorted(run["plain"]))
                for file in (fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV):
                    self.assertEqual(run["engine"][file], run["plain"][file], file)
                self.assertEqual(without_engine_fit(run["engine"][fcw.OUTPUT_WEIGHTS]),
                                 run["plain"][fcw.OUTPUT_WEIGHTS])
                record = json.loads(run["engine"][fcw.OUTPUT_WEIGHTS])["provenance"][cef.PROVENANCE_KEY]
                self.assertEqual(record, run["summary"]["engine_fit"])
                self.assertEqual((record["status"], record["written"]), ("agree", cef.WRITTEN))
                self.assertEqual((record["max_abs_factor_diff"], record["admission_max_rel_float_diff"],
                                  record["fit_max_rel_float_diff"]), (0.0, 0.0, 0.0))
                self.assertNotIn("engine_fit", run["plain_summary"])
                self.assertNotIn(cef.PROVENANCE_KEY, json.loads(run["plain"][fcw.OUTPUT_WEIGHTS])["provenance"])

    def test_every_case_exercises_its_blocks(self):
        blocks = {name: run["summary"]["engine_fit"]["blocks"] for name, run in self.runs.items()}
        self.assertEqual(blocks, {"std-erc-tsmom": ["theme_erc", "theme_tsmom"], "shrink-tsmom": ["theme_tsmom"],
                                  "std-aim-resid": ["theme_resid"]})

    def run_one(self, name: str, tag: str, **mutations) -> tuple[Path, object]:
        """One --engine-fit run of CASES[name] with stand-in mutations; (output dir, (code, summary) or the error)."""
        out = self.root / f"{name}-{tag}"
        with StandIns(self.root / f"{name}-{tag}-factors", **mutations):
            args = self.fx.args(out, **CASES[name], **placeholders(self.root / f"{name}-{tag}-in"))
            try:
                return out, fcw.fit(args)
            except fcw.FitError as exc:
                return out, exc

    def assert_refused(self, name: str, tag: str, needle: str, **mutations) -> None:
        out, got = self.run_one(name, tag, **mutations)
        self.assertIsInstance(got, fcw.FitError, tag)
        self.assertIn(needle, str(got), tag)
        self.assertFalse(out.exists(), f"{tag}: nothing is published on a refusal")

    def test_a_nudged_weight_beyond_p12_is_refused(self):
        def nudge(doc):
            doc["weights"][0] *= 1 + 1e-9
        self.assert_refused("std-erc-tsmom", "weight", "the C++ fit differs", fit_mutate=nudge)

    def test_a_changed_decision_is_refused(self):
        def status(csv, manifest):
            lines = csv.splitlines()
            header = lines[0].split(",")
            cells = lines[1].split(",")
            col = header.index("status")
            cells[col] = "reject_veto" if cells[col] != "reject_veto" else "admitted"
            return "\n".join([lines[0], ",".join(cells)] + lines[2:]) + "\n", manifest

        def order(doc):
            doc["theme_resid"]["order"] = list(reversed(doc["theme_resid"]["order"]))
        self.assert_refused("shrink-tsmom", "status", "admission.csv differs", screen_mutate=status)
        self.assert_refused("std-aim-resid", "order", "the C++ fit differs", fit_mutate=order)

    def test_a_nudged_factor_or_a_moved_flat_day_is_refused(self):
        def factor(f):
            row, col = np.argwhere(np.isfinite(f))[0]
            f[row, col] += 1e-9

        def flat(f):  # a flat day moved: the first flat cell made live, else the first live cell made flat
            flats = np.argwhere(np.isnan(f))
            if len(flats):
                f[flats[0][0], flats[0][1]] = 0.0
            else:
                row, col = np.argwhere(np.isfinite(f))[0]
                f[row, col] = np.nan
        self.assert_refused("std-erc-tsmom", "factor", "factor series beyond", factor_mutate=factor)
        self.assert_refused("std-erc-tsmom", "flat", "flat decisions differ", factor_mutate=flat)

    def test_a_cpp_value_within_p12_is_the_value_written(self):
        """Ruling D2-1: the weights file carries the C++ values. A relative 1e-14 change of the C++ parent weights
        (ew-theme-std-v1 alone: the parent's weights are the final ones) passes the check and is what is written,
        member for member, with the theme table's member weights derived from it."""
        scale = 1 + 1e-14

        def nudge(doc):
            doc["parent"]["weights"] = [w * scale for w in doc["parent"]["weights"]]
            doc["weights"] = [w * scale for w in doc["weights"]]
        plain = self.root / "std-only-plain"
        self.assertEqual(fcw.fit(self.fx.args(plain, **STD_ARGS))[0], fcw.EXIT_OK)
        out = self.root / "std-only-engine"
        with StandIns(self.root / "std-only-factors", fit_mutate=nudge):
            code, summary = fcw.fit(self.fx.args(out, **STD_ARGS, **placeholders(self.root / "std-only-in")))
        self.assertEqual(code, fcw.EXIT_OK)
        want = json.loads(load(plain)[fcw.OUTPUT_WEIGHTS])
        got = json.loads(load(out)[fcw.OUTPUT_WEIGHTS])
        weighted = [i for i, w in want["weights"].items() if w > 0]
        self.assertTrue(weighted)
        for cid in weighted:
            self.assertEqual(got["weights"][cid], want["weights"][cid] * scale, cid)
            self.assertNotEqual(got["weights"][cid], want["weights"][cid], cid)
        for theme, entry in got["provenance"]["themes"].items():
            for cid, w in entry["member_weights"].items():
                self.assertEqual(w, got["weights"][cid], (theme, cid))
        self.assertGreater(summary["engine_fit"]["fit_max_rel_float_diff"], 0.0)
        self.assertLessEqual(summary["engine_fit"]["fit_max_rel_float_diff"], cef.P12_TOLERANCE)
        by_block = summary["engine_fit"]["fit_max_pure_rel_diff_by_block"]      # review S-8 (report only)
        self.assertEqual(sorted(by_block), ["parent", "weights"])
        self.assertGreater(by_block["weights"], 0.0)

    def test_provenance_names_the_producing_code(self):
        """Review S-3: provenance.engine_fit names both executables by SHA-256 and the turning code by fingerprint."""
        record = json.loads(self.runs["std-erc-tsmom"]["engine"][fcw.OUTPUT_WEIGHTS])["provenance"]["engine_fit"]
        empty = hashlib.sha256(b"").hexdigest()                                 # the placeholders are empty files
        self.assertEqual((record["admission_exe_sha256"], record["composition_exe_sha256"]), (empty, empty))
        self.assertEqual(record["producer_fingerprint"], cef.producer_fingerprint())
        self.assertEqual(cef.PRODUCERS, ("verify", "kernels", "check_written"))

    def test_ic_shrink_states_its_ics_are_the_cpp_values(self):
        """Ruling D2-S4: under the flag the ic-shrink block says its ICs are the C++ fit's and records the largest
        |C++ IC - admission train_mean| (<= P12); admission.json is the flag-absent one."""
        run = self.runs["shrink-tsmom"]
        shrink = json.loads(run["engine"][fcw.OUTPUT_WEIGHTS])["provenance"]["ic_shrink"]
        self.assertEqual(shrink["ic_source"], cis.IC_SOURCE_ENGINE)
        self.assertEqual(shrink["ic_admission_max_abs_diff"], 0.0)              # stand-ins: the Python's bits
        plain = json.loads(run["plain"][fcw.OUTPUT_WEIGHTS])["provenance"]["ic_shrink"]
        self.assertEqual(plain["ic_source"], cis.IC_SOURCE)
        self.assertNotIn("ic_admission_max_abs_diff", plain)
        self.assertEqual(run["engine"][fcw.OUTPUT_ADMISSION], run["plain"][fcw.OUTPUT_ADMISSION])

        def shift(doc):  # every C++ IC 1e-9 away from the admission train_mean: beyond P12
            doc["parent"]["ic"] = [x + 1e-9 for x in doc["parent"]["ic"]]
        self.assert_refused("shrink-tsmom", "ic-shift", "the C++ fit differs", fit_mutate=shift)


class EngineFitResidParent(unittest.TestCase):
    """Ruling D2-R2: --theme-resid with --theme-resid-parent when either fit runs --engine-fit. The parent is an
    ic-shrink-v1 cell (the rule whose ICs the flag changes); the engine fits' C++ values are the Python's times
    1 + 1e-14 (rounding inside P12). composition_resid.parent_check sets provenance.engine_fit and the IC annotations
    aside and compares the rest under P12; a fitter change beyond P12 is still refused, before the executables run."""
    SCALE = 1 + 1e-14
    SHRINK = dict(STD_ARGS, composition=cis.RULE_ID)

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.fx = World.build(cls.root)
        registry = unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json")
        registry.start()
        cls.addClassCleanup(registry.stop)
        cls.addClassCleanup(cls.tmp.cleanup)
        assert fcw.fit(cls.fx.args(cls.root / "parent-plain", **cls.SHRINK))[0] == fcw.EXIT_OK
        with StandIns(cls.root / "parent-engine-factors", fit_mutate=nudge_floats(cls.SCALE)):
            code, _ = fcw.fit(cls.fx.args(cls.root / "parent-engine", **cls.SHRINK,
                                          **placeholders(cls.root / "parent-engine-in")))
        assert code == fcw.EXIT_OK

    def child(self, name: str, parent: str, engine: bool, **patches):
        """The resid re-fit of the parent cell's argv into ``name``: (output dir, (code, summary) or the FitError,
        the executables' verbs run)."""
        weights = self.root / parent / fcw.OUTPUT_WEIGHTS
        args = dict(self.SHRINK, theme_resid=cres.RULE_ID, theme_resid_parent=str(weights),
                    theme_resid_parent_sha256=hashlib.sha256(weights.read_bytes()).hexdigest())
        out = self.root / name
        stand = StandIns(self.root / f"{name}-factors", fit_mutate=nudge_floats(self.SCALE))
        with contextlib.ExitStack() as stack:
            for target, attr, value in patches.get("patch", ()):
                stack.enter_context(unittest.mock.patch.object(target, attr, value))
            if engine:
                stack.enter_context(stand)
                args.update(placeholders(self.root / f"{name}-in"))
            try:
                got = fcw.fit(self.fx.args(out, **args))
            except fcw.FitError as exc:
                got = exc
        return out, got, stand.calls

    def assert_child_accepted(self, out: Path, got, parent: str, engine_mode: bool) -> dict:
        self.assertNotIsInstance(got, Exception, str(got))
        self.assertEqual(got[0], fcw.EXIT_OK)
        doc = json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())
        resid = doc["provenance"]["resid"]
        self.assertEqual(resid["parent_check"], cres.PARENT_CHECK)
        self.assertEqual(resid.get("parent_check_engine_fit"), cres.PARENT_CHECK_ENGINE_FIT if engine_mode else None)
        self.assertEqual((out / fcw.OUTPUT_ADMISSION).read_bytes(),
                         (self.root / parent / fcw.OUTPUT_ADMISSION).read_bytes())
        return doc

    def test_engine_parent_and_engine_child(self):
        out, got, calls = self.child("ee", "parent-engine", True)
        doc = self.assert_child_accepted(out, got, "parent-engine", True)
        self.assertEqual(calls, ["screen", "fit"])
        parent = json.loads((self.root / "parent-engine" / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertEqual(doc["weights"], parent["weights"])                     # the same C++ values both times
        self.assertEqual(doc["provenance"]["ic_shrink"]["ic_source"], cis.IC_SOURCE_ENGINE)

    def test_engine_parent_and_plain_child(self):
        out, got, calls = self.child("pe", "parent-engine", False)
        doc = self.assert_child_accepted(out, got, "parent-engine", True)
        self.assertEqual(calls, [])
        parent = json.loads((self.root / "parent-engine" / fcw.OUTPUT_WEIGHTS).read_bytes())
        weighted = [i for i, w in parent["weights"].items() if w > 0]
        self.assertTrue(any(doc["weights"][i] != parent["weights"][i] for i in weighted))   # Python vs C++ values
        self.assertNotIn(cef.PROVENANCE_KEY, doc["provenance"])

    def test_plain_parent_and_engine_child_writes_the_cpp_values(self):
        """The second (C++-valued) pass is checked under P12 and passes; its values are the ones written."""
        out, got, calls = self.child("ep", "parent-plain", True)
        doc = self.assert_child_accepted(out, got, "parent-plain", True)
        self.assertEqual(calls, ["screen", "fit"])
        parent = json.loads((self.root / "parent-plain" / fcw.OUTPUT_WEIGHTS).read_bytes())
        for cid, w in parent["weights"].items():
            self.assertEqual(doc["weights"][cid], w * self.SCALE, cid)

    def test_plain_parent_and_plain_child_is_byte_exact_as_before(self):
        out, got, _ = self.child("pp", "parent-plain", False)
        doc = self.assert_child_accepted(out, got, "parent-plain", False)
        del doc[cres.BLOCK]
        del doc["provenance"]["resid"]
        self.assertEqual(fcw.canonical_bytes(doc), (self.root / "parent-plain" / fcw.OUTPUT_WEIGHTS).read_bytes())

    def test_a_fitter_change_is_still_refused_naming_the_cause(self):
        """A changed shrinkage intensity moves every share far beyond P12: refused in the first (Python) pass, before
        either executable runs, with a message naming --engine-fit and ruling P12; nothing is published."""
        real = cis.shrunk_shares

        def changed(themes, ics, intensity=cis.INTENSITY, floor=cis.FLOOR):
            return real(themes, ics, 0.25, floor)
        for name, parent, engine, sides in (("ee-changed", "parent-engine", True, "the parent and this re-fit"),
                                            ("pe-changed", "parent-engine", False, "the parent"),
                                            ("ep-changed", "parent-plain", True, "this re-fit")):
            with self.subTest(name):
                out, got, calls = self.child(name, parent, engine, patch=[(cis, "shrunk_shares", changed)])
                self.assertIsInstance(got, fcw.FitError)
                self.assertIn(f"--engine-fit on {sides} (ruling D2-R2)", str(got))
                self.assertIn("within ruling P12", str(got))
                self.assertRegex(str(got), r"\(\d+ values differ: \['\$\.")
                self.assertEqual(calls, [])
                self.assertFalse(out.exists())
        out, got, _ = self.child("pp-changed", "parent-plain", False, patch=[(cis, "shrunk_shares", changed)])
        self.assertIsInstance(got, fcw.FitError)                                 # flag absent: the v8 exact check
        self.assertIn("a fitter-affecting change lies between the two fits", str(got))
        self.assertFalse(out.exists())


class CheckArgs(unittest.TestCase):
    def ns(self, **kw) -> argparse.Namespace:
        base = dict(engine_fit=False, engine_factors=None, admission_exe=None, composition_exe=None,
                    composition="ew-theme-std-v1")
        return argparse.Namespace(**dict(base, **kw))

    def test_refusals_before_any_compute(self):
        with tempfile.TemporaryDirectory() as tmp:
            ready = placeholders(Path(tmp))
            cef.check_args(self.ns(), prior=True, pooled=False)                   # flag absent: nothing checked
            cef.check_args(self.ns(**ready), prior=True, pooled=False)            # every input present
            refused = {
                "options without the flag": self.ns(engine_factors=ready["engine_factors"]),
                "ew-theme-v1": self.ns(**dict(ready, composition="ew-theme-v1")),
                "ew-theme-aim-v2": self.ns(**dict(ready, composition="ew-theme-aim-v2")),
                "no factors": self.ns(**dict(ready, engine_factors=None)),
                "no exe": self.ns(**dict(ready, composition_exe=Path(tmp) / "missing.exe")),
                "no manifest": self.ns(**dict(ready, engine_factors=Path(tmp))),
            }
            for what, ns in refused.items():
                with self.subTest(what), self.assertRaises(cef.EngineFitError):
                    cef.check_args(ns, prior=True, pooled=False)
            with self.assertRaises(cef.EngineFitError):                           # the train orientation
                cef.check_args(self.ns(**ready), prior=False, pooled=False)
            with self.assertRaises(cef.EngineFitError):                           # an era pool
                cef.check_args(self.ns(**ready), prior=True, pooled=True)

    def test_the_fitter_refuses_through_its_own_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fx = World.build(root)
            args = fx.args(root / "out", **dict(STD_ARGS, composition="ew-theme-v1"), **placeholders(root / "in"))
            with self.assertRaises(fcw.FitError) as caught:
                fcw.fit(args)
            self.assertIn("--engine-fit", str(caught.exception))
            self.assertFalse((root / "out").exists())


class Comparisons(unittest.TestCase):
    def test_compare_values(self):
        want = {"a": [1.0, 2, "x", None, True], "b": {"c": 0.5}}
        self.assertEqual(cef.compare_values(want, json.loads(json.dumps(want))), ([], 0.0))
        diff, worst = cef.compare_values(want, {"a": [1.0 + 1e-13, 2, "x", None, True], "b": {"c": 0.5}})
        self.assertEqual(diff, [])
        self.assertAlmostEqual(worst, 1e-13, delta=1e-15)
        self.assertTrue(cef.compare_values(want, {"a": [1.0 + 1e-11, 2, "x", None, True], "b": {"c": 0.5}})[0])
        self.assertTrue(cef.compare_values(want, {"a": [1.0, 3, "x", None, True], "b": {"c": 0.5}})[0])   # integers
        self.assertTrue(cef.compare_values(want, {"a": [1.0, 2, "y", None, True], "b": {"c": 0.5}})[0])   # strings
        self.assertTrue(cef.compare_values(want, {"a": [1.0, 2, "x", None], "b": {"c": 0.5}})[0])         # length
        self.assertTrue(cef.compare_values(want, {"a": [1.0, 2, "x", None, True], "b": {}})[0])           # keys
        self.assertEqual(cef.compare_values({"x": 1e-300}, {"x": 2e-300})[0], [])    # relative to max(1, |x|)
        self.assertTrue(cef.compare_values({"x": float("inf")}, {"x": 1e308})[0])

    def test_compare_admission_csv(self):
        csv = "id,status,tau,train_mean\na,admitted,0.1,0.002\nb,reject_veto,0.2,-0.001\n"
        self.assertEqual(cef.compare_admission_csv(csv.encode(), csv.encode()), ([], 0.0))
        near = csv.replace("0.002", repr(0.002 * (1 + 1e-14)))
        self.assertEqual(cef.compare_admission_csv(csv.encode(), near.encode())[0], [])
        self.assertTrue(cef.compare_admission_csv(csv.encode(), csv.replace("0.002", "0.0021").encode())[0])
        self.assertTrue(cef.compare_admission_csv(csv.encode(), csv.replace("reject_veto", "admitted").encode())[0])
        self.assertTrue(cef.compare_admission_csv(csv.encode(), csv.replace("b,", "c,").encode())[0])
        self.assertTrue(cef.compare_admission_csv(csv.encode(), csv.rsplit("\n", 2)[0].encode())[0])


class ModuleFingerprints(unittest.TestCase):
    """Ruling D2-2 (CM-5 for the composition modules): each provenance block names its writer by
    composition_rules.module_fingerprint, which moves with code and constants the block's numbers depend on (its own
    module's and the composition modules it reads) and not with comments or formatting."""
    MODULES = {"composition_rules.py": cr, "composition_ic_shrink.py": cis, "composition_theme_erc.py": cte,
               "composition_theme_tsmom.py": ct, "composition_resid.py": cres, "composition_engine_fit.py": cef}

    def test_every_module_names_its_writer(self):
        for name, module in self.MODULES.items():
            with self.subTest(name):
                fp = module.producer_fingerprint()
                self.assertRegex(fp, r"^[0-9a-f]{64}$")
                self.assertEqual(fp, cr.module_fingerprint(module.__file__, module.PRODUCERS))
                self.assertFalse(hasattr(module, "module_sha256"))
        self.assertEqual(len({m.producer_fingerprint() for m in self.MODULES.values()}), len(self.MODULES))

    def test_comments_do_not_move_it_and_code_does(self):
        tools = Path(cr.__file__).resolve().parent
        with tempfile.TemporaryDirectory() as tmp:
            def copy(tag: str, edit: dict | None = None) -> Path:
                d = Path(tmp) / tag
                d.mkdir()
                for name in self.MODULES:
                    text = (tools / name).read_text(encoding="utf-8")
                    for old, new in (edit or {}).get(name, ()):
                        self.assertIn(old, text, (name, old))
                        text = text.replace(old, new, 1)
                    (d / name).write_text(text, encoding="utf-8")
                return d / "composition_ic_shrink.py"

            def fp(path: Path) -> str:
                return cr.module_fingerprint(str(path), cis.PRODUCERS)

            base = fp(copy("base"))
            self.assertEqual(base, cis.producer_fingerprint())
            comment = {"composition_ic_shrink.py": [("def shrunk_shares(", "# a comment\ndef shrunk_shares(")],
                       "composition_rules.py": [("def member_cap(", "# a comment\ndef member_cap(")]}
            self.assertEqual(fp(copy("comment", comment)), base)
            own = {"composition_ic_shrink.py": [("\nINTENSITY = 0.5 ", "\nINTENSITY = 0.25 ")]}
            self.assertNotEqual(fp(copy("own", own)), base)
            read = {"composition_rules.py": [("\nCAP_TOLERANCE = 1e-12 ", "\nCAP_TOLERANCE = 1e-11 ")]}
            self.assertNotEqual(fp(copy("read", read)), base)      # composition_rules.member_cap's constant
            # theme-resid's parent check (in apply's closure) reads composition_engine_fit's P12 comparison
            resid = Path(tmp) / "base" / "composition_resid.py"
            p12 = Path(tmp) / "p12" / "composition_resid.py"
            copy("p12", {"composition_engine_fit.py": [("\nP12_TOLERANCE = 1e-12 ", "\nP12_TOLERANCE = 1e-11 ")]})
            self.assertEqual(cr.module_fingerprint(str(resid), cres.PRODUCERS), cres.producer_fingerprint())
            self.assertNotEqual(cr.module_fingerprint(str(p12), cres.PRODUCERS), cres.producer_fingerprint())


@unittest.skipUnless(os.environ.get("ATX_RESEARCH_ADMISSION_EXE") and os.environ.get("ATX_RESEARCH_COMPOSITION_EXE"),
                     "needs built atx-research-admission and atx-research-composition")
class RealExecutables(unittest.TestCase):
    """The built executables on the fitter's own series (the directory written from the fitter's state): the check
    agrees under P12 and the weights file carries the C++ values, each within P12 of the flag-absent file's."""

    def test_real_executables_agree_and_are_written(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fx = World.build(root)
            exes = dict(admission_exe=Path(os.environ["ATX_RESEARCH_ADMISSION_EXE"]),
                        composition_exe=Path(os.environ["ATX_RESEARCH_COMPOSITION_EXE"]))
            real_check = cef.check_factor_series

            def check(directory, state, error):
                write_factor_dir(Path(directory), state)
                return real_check(directory, state, error)

            with unittest.mock.patch.object(cr, "REGISTRY_PATH", root / "no-registry.json"):
                for name, case in CASES.items():
                    with self.subTest(name):
                        self.assertEqual(fcw.fit(fx.args(root / f"{name}-plain", **case))[0], fcw.EXIT_OK)
                        ready = dict(placeholders(root / f"{name}-in"), **exes)
                        with unittest.mock.patch.object(cef, "check_factor_series", check):
                            code, summary = fcw.fit(fx.args(root / f"{name}-engine", **case, **ready))
                        self.assertEqual(code, fcw.EXIT_OK)
                        self.assertEqual(summary["engine_fit"]["status"], "agree")
                        plain, engine = load(root / f"{name}-plain"), load(root / f"{name}-engine")
                        self.assertEqual(engine[fcw.OUTPUT_ADMISSION], plain[fcw.OUTPUT_ADMISSION])
                        want = json.loads(plain[fcw.OUTPUT_WEIGHTS])
                        got = json.loads(without_engine_fit(engine[fcw.OUTPUT_WEIGHTS]))
                        self.assertEqual(cef.compare_values(want, got)[0], [])


if __name__ == "__main__":
    unittest.main()
