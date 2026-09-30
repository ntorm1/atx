"""Synthetic checks for compare_window_overlap (no real data; every session is before 2024-01-01).

Run: python -m pytest atx-impl/tools/test_compare_window_overlap.py -q
"""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_window_overlap as cwo  # noqa: E402

DAY = cwo.DAY_NS
OLD_SESSIONS = ["2021-01-04", "2021-01-05", "2021-01-06", "2021-01-07", "2021-01-08",
                "2021-01-11", "2021-01-12", "2021-01-13", "2021-01-14", "2021-01-15"]
NEW_SESSIONS = OLD_SESSIONS + ["2021-01-19", "2021-01-20", "2021-01-21", "2021-01-22"]
OLD_IDS = [10, 20, 30, 40]
NEW_IDS = [5, 10, 15, 20, 25, 30, 40, 50]  # the union grows and the new columns interleave with the old ones


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def ns(text: str) -> int:
    return (dt.date.fromisoformat(text) - dt.date(1970, 1, 1)).days * DAY


def value(session: str, sid: int) -> float:
    """One deterministic value per (session, id), so both windows agree wherever they overlap."""
    d = ns(session) // DAY
    return math.nan if (d + sid) % 7 == 0 else math.sin(0.1 * d + 0.37 * sid) * 100.0


def panel(sessions, ids) -> np.ndarray:
    return np.array([[value(s, i) for i in ids] for s in sessions], dtype="<f8")


def write_role(path: Path, sessions, ids) -> str:
    path.mkdir(parents=True)
    s = np.array([ns(x) for x in sessions], dtype="<i8").tobytes()
    i = np.array(ids, dtype="<u8").tobytes()
    (path / "sessions.i64").write_bytes(s)
    (path / "ids.u64").write_bytes(i)
    manifest = {"schema": cwo.ROLE_SCHEMA, "status": "complete", "dates": len(sessions), "instruments": len(ids),
                "files": {"sessions.i64": {"bytes": len(s), "sha256": sha(s)},
                          "ids.u64": {"bytes": len(i), "sha256": sha(i)}}}
    raw = json.dumps(manifest).encode()
    (path / "manifest.json").write_bytes(raw)
    return sha(raw)


def write_fields(path: Path, role: Path, fields: dict) -> None:
    path.mkdir(parents=True)
    rm = json.loads((role / "manifest.json").read_bytes())
    entries, files = [], {}
    for name, values in fields.items():
        data = np.ascontiguousarray(values, dtype="<f8").tobytes()
        (path / f"{name}.f64").write_bytes(data)
        files[f"{name}.f64"] = {"bytes": len(data), "sha256": sha(data)}
        entries.append({"name": name, "file": f"{name}.f64", "dtype": "<f8", "layout": "date-major",
                        "shape": [rm["dates"], rm["instruments"]], "sha256": sha(data)})
    manifest = {"schema": cwo.FIELDS_SCHEMA, "status": "complete",
                "role": {"path": str(role), "manifest_sha256": sha((role / "manifest.json").read_bytes()),
                         "sessions_sha256": rm["files"]["sessions.i64"]["sha256"],
                         "ids_sha256": rm["files"]["ids.u64"]["sha256"], "dates": rm["dates"],
                         "instruments": rm["instruments"]},
                "fields": entries, "files": files}
    (path / "manifest.json").write_text(json.dumps(manifest))


def write_signal(root: Path, rel: str, cid: str, dsl_sha: str, role_sha: str, values: np.ndarray,
                 schema: str = "atx.dsl-candidate-signal/v2") -> str:
    """A cache entry at root/rel/<cid>.<dsl16>.{f64,json}, as cache_store writes it (the fields the tool reads)."""
    where = root / rel
    where.mkdir(parents=True, exist_ok=True)
    stem = f"{cid}.{dsl_sha[:16]}"
    data = np.ascontiguousarray(values, dtype="<f8").tobytes()
    (where / f"{stem}.f64").write_bytes(data)
    sidecar = {"schema": schema, "candidate_id": cid, "dsl_sha256": dsl_sha, "role_manifest_sha256": role_sha,
               "dates": values.shape[0], "instruments": values.shape[1], "bytes": len(data), "payload": f"{stem}.f64",
               "payload_sha256": sha(data)}
    (where / f"{stem}.json").write_text(json.dumps(sidecar))
    return sha(data)


def run(argv) -> tuple[int, dict | None, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cwo.main([str(a) for a in argv])
    report = None
    if "--out" in argv:
        path = Path(argv[argv.index("--out") + 1])
        report = json.loads(path.read_text()) if path.exists() else None
    return code, report, err.getvalue()


class Base(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.old_role, self.new_role = self.tmp / "role_old", self.tmp / "role_new"
        self.old_sha = write_role(self.old_role, OLD_SESSIONS, OLD_IDS)
        self.new_sha = write_role(self.new_role, NEW_SESSIONS, NEW_IDS)
        self.n = 0

    def tearDown(self):
        self._tmp.cleanup()

    def out(self) -> Path:
        self.n += 1
        return self.tmp / f"report{self.n}.json"

    def fields(self, new_panel=None, old_panel=None, names=("x",)):
        old_dir, new_dir = self.tmp / f"old_fields{self.n}", self.tmp / f"new_fields{self.n}"
        self.n += 1
        write_fields(old_dir, self.old_role, {k: panel(OLD_SESSIONS, OLD_IDS) if old_panel is None else old_panel
                                              for k in names})
        write_fields(new_dir, self.new_role, {k: panel(NEW_SESSIONS, NEW_IDS) if new_panel is None else new_panel
                                              for k in names})
        return old_dir, new_dir


class FieldKind(Base):
    def test_identical_overlap_with_instrument_union_growth(self):
        old_dir, new_dir = self.fields(names=("x", "y"))
        code, rep, err = run(["--kind", "field", "--old", old_dir, "--new", new_dir, "--out", self.out(), "--per-key"])
        self.assertEqual(code, 0, err)
        t = rep["totals"]
        self.assertTrue(t["bit_identical"])
        self.assertEqual((t["unequal_cells"], t["old_cells_missing_in_new"], t["max_abs_diff"]), (0, 0, None))
        self.assertEqual(t["cells_compared"], 2 * len(OLD_SESSIONS) * len(OLD_IDS))
        self.assertGreater(t["new_only_finite_cells"], 0)  # the interleaved new columns carry values
        a = rep["alignment"]
        self.assertEqual((a["common_sessions"], a["new_only_sessions"], a["common_instruments"],
                          a["new_only_instruments"], a["old_only_instruments"]), (10, 4, 4, 4, 0))
        self.assertEqual([r["key"] for r in rep["rows"]], ["x", "y"])
        self.assertEqual(rep["differing_keys"], [])
        # A positional comparison would have failed: the old columns are not a prefix of the new ones.
        self.assertFalse(np.array_equal(panel(OLD_SESSIONS, OLD_IDS), panel(OLD_SESSIONS, NEW_IDS[:4]),
                                        equal_nan=True))

    def test_changed_cell_found_with_session_and_id(self):
        new = panel(NEW_SESSIONS, NEW_IDS)
        new[NEW_SESSIONS.index("2021-01-12"), NEW_IDS.index(40)] += 0.5
        new[NEW_SESSIONS.index("2021-01-07"), NEW_IDS.index(30)] += 1e-12  # the earlier one is reported first
        new[NEW_SESSIONS.index("2021-01-20"), NEW_IDS.index(30)] += 9.0    # a new-only session: not compared
        old_dir = self.tmp / "old_mixed"
        write_fields(old_dir, self.old_role, {"x": panel(OLD_SESSIONS, OLD_IDS), "same": panel(OLD_SESSIONS, OLD_IDS)})
        # field x carries the changes, field same does not
        write_fields(self.tmp / "new_mixed", self.new_role, {"x": new, "same": panel(NEW_SESSIONS, NEW_IDS)})
        code, rep, err = run(["--kind", "field", "--old", old_dir, "--new", self.tmp / "new_mixed",
                              "--out", self.out(), "--per-key"])
        self.assertEqual(code, 0, err)
        rows = {r["key"]: r for r in rep["rows"]}
        x = rows["x"]
        self.assertFalse(x["bit_identical"])
        self.assertEqual(x["unequal_cells"], 2)
        self.assertAlmostEqual(x["max_abs_diff"], 0.5, places=12)
        self.assertEqual(x["first_diff"], {"session": "2021-01-07", "session_ns": ns("2021-01-07"),
                                           "instrument_id": 30})
        self.assertTrue(rows["same"]["bit_identical"])
        t = rep["totals"]
        self.assertEqual((t["unequal_cells"], t["bit_identical"]), (2, False))
        self.assertEqual(t["first_diff"]["key"], "x")
        self.assertEqual(rep["differing_keys"], ["x"])

    def test_nan_handling(self):
        old, new = panel(OLD_SESSIONS, OLD_IDS), panel(NEW_SESSIONS, NEW_IDS)
        r, c_old, c_new = OLD_SESSIONS.index("2021-01-05"), OLD_IDS.index(20), NEW_IDS.index(20)
        old[r, c_old] = np.nan
        new[r, c_new] = np.array([0x7FF8_0000_0000_0001], dtype="<u8").view("<f8")[0]  # another NaN payload: equal
        r2 = OLD_SESSIONS.index("2021-01-13")
        old[r2, OLD_IDS.index(10)] = 3.0
        new[r2, NEW_IDS.index(10)] = np.nan                                             # NaN vs value: unequal
        self.assertNotEqual(new.view("<u8")[r, c_new], old.view("<u8")[r, c_old])  # different NaN bits
        old_dir, new_dir = self.fields(new_panel=new, old_panel=old)
        code, rep, err = run(["--kind", "field", "--old", old_dir, "--new", new_dir, "--out", self.out(), "--per-key"])
        self.assertEqual(code, 0, err)
        row = rep["rows"][0]
        self.assertEqual((row["unequal_cells"], row["nan_mismatch_cells"], row["max_abs_diff"]), (1, 1, None))
        self.assertEqual(row["first_diff"]["session"], "2021-01-13")
        self.assertEqual(row["first_diff"]["instrument_id"], 10)
        self.assertFalse(row["bit_identical"])

    def test_old_value_without_new_cell_is_missing(self):
        # The new role drops id 40: its finite old values have no aligned cell.
        ids = [i for i in NEW_IDS if i != 40]
        role = self.tmp / "role_drop"
        write_role(role, NEW_SESSIONS, ids)
        old_dir = self.tmp / "old_drop"
        write_fields(old_dir, self.old_role, {"x": panel(OLD_SESSIONS, OLD_IDS)})
        write_fields(self.tmp / "new_drop", role, {"x": panel(NEW_SESSIONS, ids)})
        code, rep, err = run(["--kind", "field", "--old", old_dir, "--new", self.tmp / "new_drop",
                              "--out", self.out()])
        self.assertEqual(code, 0, err)
        t = rep["totals"]
        finite = int(np.isfinite(panel(OLD_SESSIONS, [40])).sum())
        self.assertEqual((t["unequal_cells"], t["old_cells_missing_in_new"]), (0, finite))
        self.assertFalse(t["bit_identical"])
        self.assertNotIn("rows", rep)  # rows only with --per-key

    def test_before_limits_the_compared_sessions(self):
        new = panel(NEW_SESSIONS, NEW_IDS)
        new[NEW_SESSIONS.index("2021-01-14"), NEW_IDS.index(20)] += 2.0
        old_dir, new_dir = self.fields(new_panel=new)
        code, rep, err = run(["--kind", "field", "--old", old_dir, "--new", new_dir, "--out", self.out(),
                              "--before", "2021-01-14"])
        self.assertEqual(code, 0, err)
        self.assertTrue(rep["totals"]["bit_identical"])
        self.assertEqual(rep["alignment"]["common_sessions"], 8)
        self.assertEqual(rep["alignment"]["last_common_session"], "2021-01-13")
        code, rep, err = run(["--kind", "field", "--old", old_dir, "--new", new_dir, "--out", self.out(),
                              "--before", "2021-01-15"])
        self.assertEqual(code, 0, err)
        self.assertEqual(rep["totals"]["unequal_cells"], 1)
        self.assertEqual(rep["totals"]["first_diff"]["session"], "2021-01-14")

    def test_seal_refuses_2024_sessions_and_before(self):
        role = self.tmp / "role_sealed"
        write_role(role, NEW_SESSIONS + ["2024-01-02"], NEW_IDS)
        old_dir = self.tmp / "old_sealed"
        write_fields(old_dir, self.old_role, {"x": panel(OLD_SESSIONS, OLD_IDS)})
        write_fields(self.tmp / "new_sealed", role, {"x": panel(NEW_SESSIONS + ["2024-01-02"], NEW_IDS)})
        out = self.out()
        code, rep, err = run(["--kind", "field", "--old", old_dir, "--new", self.tmp / "new_sealed", "--out", out])
        self.assertEqual(code, 2)
        self.assertIn("seal 2024-01-01", err)
        self.assertIsNone(rep)
        old_dir, new_dir = self.fields()
        code, rep, err = run(["--kind", "field", "--old", old_dir, "--new", new_dir, "--out", self.out(),
                              "--before", "2024-01-02"])
        self.assertEqual(code, 2)
        self.assertIn("after the seal", err)

    def test_zero_cells_compared_is_never_identical(self):
        """Review C-9: a cutoff on or before the first common session, or roles sharing no instrument (old cells all
        NaN), compare no cell: bit_identical is false with a reason, and W0-a reads stop."""
        old_dir, new_dir = self.fields(names=("x", "y"))
        code, rep, err = run(["--kind", "field", "--old", old_dir, "--new", new_dir, "--out", self.out(), "--per-key",
                              "--before", OLD_SESSIONS[0]])
        self.assertEqual(code, 0, err)
        self.assertEqual(rep["alignment"]["common_sessions"], 0)
        for r in rep["rows"]:
            self.assertEqual((r["cells_compared"], r["unequal_cells"], r["old_cells_missing_in_new"]), (0, 0, 0))
            self.assertFalse(r["bit_identical"])
            self.assertTrue(r["reason"].startswith("no cell compared"))
        t = rep["totals"]
        self.assertFalse(t["bit_identical"])
        self.assertIn("no cell compared", t["reason"])
        self.assertEqual(t["w0a_class"], "stop")
        self.assertEqual(rep["differing_keys"], ["x", "y"])
        # no common instrument, the old values all NaN: nothing is missing, nothing is compared
        role = self.tmp / "role_disjoint"
        ids = [1, 2, 3]
        write_role(role, NEW_SESSIONS, ids)
        old_nan = self.tmp / "old_nan"
        write_fields(old_nan, self.old_role, {"x": np.full((len(OLD_SESSIONS), len(OLD_IDS)), np.nan)})
        write_fields(self.tmp / "new_disjoint", role, {"x": panel(NEW_SESSIONS, ids)})
        out = self.out()
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code = cwo.main([str(a) for a in ("--kind", "field", "--old", old_nan, "--new", self.tmp / "new_disjoint",
                                              "--out", out)])
        self.assertEqual(code, 0)
        rep = json.loads(out.read_text())
        self.assertEqual((rep["alignment"]["common_instruments"], rep["totals"]["cells_compared"]), (0, 0))
        self.assertFalse(rep["totals"]["bit_identical"])
        line = json.loads(stdout.getvalue())
        self.assertEqual((line["bit_identical"], line["w0a_class"]), (False, "stop"))
        self.assertIn("no cell compared", line["reason"])

    def test_nan_against_value_is_a_mismatch_and_nan_against_nan_a_match(self):
        """Review C-10: a value that appears or vanishes (NaN on one side) is an unequal cell and a W0-a stop even
        with no finite difference; NaN on both sides (any payload) is equal; W0-a separates 1e-12 noise from a stop."""
        r, c_old, c_new = OLD_SESSIONS.index("2021-01-05"), OLD_IDS.index(20), NEW_IDS.index(20)
        r2, c2_old, c2_new = OLD_SESSIONS.index("2021-01-13"), OLD_IDS.index(10), NEW_IDS.index(10)
        r3, c3_old, c3_new = OLD_SESSIONS.index("2021-01-08"), OLD_IDS.index(30), NEW_IDS.index(30)

        def case(nan_nan=False, nan_value=False, noise=0.0):
            old, new = panel(OLD_SESSIONS, OLD_IDS), panel(NEW_SESSIONS, NEW_IDS)
            if nan_nan:
                old[r, c_old] = np.nan
                new[r, c_new] = np.array([0x7FF8_0000_0000_0001], dtype="<u8").view("<f8")[0]
            if nan_value:
                old[r2, c2_old], new[r2, c2_new] = 3.0, np.nan
            if noise:
                self.assertTrue(math.isfinite(old[r3, c3_old]))
                new[r3, c3_new] = old[r3, c3_old] + noise
            old_dir, new_dir = self.fields(new_panel=new, old_panel=old)
            stdout = io.StringIO()
            out = self.out()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(cwo.main([str(a) for a in ("--kind", "field", "--old", old_dir, "--new", new_dir,
                                                            "--out", out)]), 0)
            return json.loads(out.read_text())["totals"], json.loads(stdout.getvalue()), old[r3, c3_old]

        t, line, _ = case(nan_nan=True)                                        # NaN against NaN: a match
        self.assertTrue(t["bit_identical"])
        self.assertEqual((t["unequal_cells"], t["nan_mismatch_cells"], t["w0a_class"], t["reason"]),
                         (0, 0, "identical", None))
        t, line, _ = case(nan_nan=True, nan_value=True)                        # NaN against a value: a mismatch
        self.assertFalse(t["bit_identical"])
        self.assertEqual((t["unequal_cells"], t["nan_mismatch_cells"], t["max_abs_diff"]), (1, 1, None))
        self.assertEqual(t["w0a_class"], "stop")                               # never read on max_abs_diff alone
        self.assertEqual((line["nan_mismatch_cells"], line["w0a_class"]), (1, "stop"))
        self.assertIn("NaN against a value", t["reason"])
        t, line, value = case(noise=1e-12)                                     # finite noise below the tolerance
        self.assertEqual((t["unequal_cells"], t["nan_mismatch_cells"], t["w0a_class"]), (1, 0, "below-tolerance"))
        self.assertAlmostEqual(t["max_rel_diff"], abs(t["max_abs_diff"]) / abs(value), places=18)
        t, _, _ = case(nan_value=True, noise=1e-12)                            # noise plus a vanished value
        self.assertEqual((t["w0a_class"], t["nan_mismatch_cells"]), ("stop", 1))
        self.assertLess(t["max_abs_diff"], cwo.W0A_TOLERANCE)                  # what the old reading saw
        t, _, _ = case(noise=1e-6)
        self.assertEqual(t["w0a_class"], "stop")

    def test_role_must_be_the_bound_one_and_output_never_overwritten(self):
        old_dir, new_dir = self.fields()
        code, _, err = run(["--kind", "field", "--old", old_dir, "--new", new_dir, "--out", self.out(),
                            "--new-role", self.old_role])
        self.assertEqual(code, 2)
        self.assertIn("not the role the manifest is bound to", err)
        out = self.out()
        out.write_text("{}")
        code, _, err = run(["--kind", "field", "--old", old_dir, "--new", new_dir, "--out", out])
        self.assertEqual(code, 2)
        self.assertIn("refusing overwrite", err)


class SignalKind(Base):
    def caches(self):
        """Old and new caches; the new one keys the same candidates under other field-payload directories."""
        old_root, new_root = self.tmp / "cache_old", self.tmp / "cache_new"
        dsl_a, dsl_b = sha(b"rank(close)"), sha(b"rank(volume)")
        old_a = panel(OLD_SESSIONS, OLD_IDS)
        new_a = panel(NEW_SESSIONS, NEW_IDS)
        old_b = -panel(OLD_SESSIONS, OLD_IDS)
        new_b = -panel(NEW_SESSIONS, NEW_IDS)
        new_b[NEW_SESSIONS.index("2021-01-08"), NEW_IDS.index(30)] += 1e-3
        vm = "dslvm1_clang18.1_fma"
        write_signal(old_root, f"{vm}/{self.old_sha}", "alpha", dsl_a, self.old_sha, old_a)
        write_signal(old_root, f"{vm}/{self.old_sha}/fp_0123456789abcdef", "beta", dsl_b, self.old_sha, old_b)
        write_signal(new_root, f"{vm}/{self.new_sha}", "alpha", dsl_a, self.new_sha, new_a)
        write_signal(new_root, f"{vm}/{self.new_sha}/fp_fedcba9876543210", "beta", dsl_b, self.new_sha, new_b)
        # Another role's entry in the old cache and an IC-result record: both ignored.
        write_signal(old_root, f"{vm}/{'e' * 64}", "alpha", dsl_a, "e" * 64, np.zeros((2, 2)))
        ic_dir = old_root / vm / self.old_sha / "ic1_0123456789abcdef"
        ic_dir.mkdir(parents=True)
        (ic_dir / f"alpha.{dsl_a[:16]}.json").write_text(json.dumps({"schema": "atx.dsl-candidate-ic/v1"}))
        return old_root, new_root, dsl_b

    def test_matched_by_candidate_id_and_changed_cell_found(self):
        old_root, new_root, _ = self.caches()
        code, rep, err = run(["--kind", "signal", "--old", old_root, "--new", new_root, "--old-role", self.old_role,
                              "--new-role", self.new_role, "--out", self.out(), "--per-key"])
        self.assertEqual(code, 0, err)
        rows = {r["key"]: r for r in rep["rows"]}
        self.assertEqual(sorted(rows), ["alpha", "beta"])
        self.assertTrue(rows["alpha"]["bit_identical"])
        self.assertTrue(rows["alpha"]["dsl_sha256_equal"])
        self.assertIn("fp_0123456789abcdef", rows["beta"]["old_sidecar"])
        self.assertIn("fp_fedcba9876543210", rows["beta"]["new_sidecar"])
        self.assertEqual(rows["beta"]["unequal_cells"], 1)
        self.assertEqual(rows["beta"]["first_diff"]["session"], "2021-01-08")
        self.assertEqual(rows["beta"]["first_diff"]["instrument_id"], 30)
        self.assertAlmostEqual(rows["beta"]["max_abs_diff"], 1e-3, places=9)
        self.assertEqual(rep["differing_keys"], ["beta"])
        self.assertIn("candidate id", rep["mapping"])

    def test_role_arguments_are_required(self):
        old_root, new_root, _ = self.caches()
        code, _, err = run(["--kind", "signal", "--old", old_root, "--new", new_root, "--out", self.out()])
        self.assertEqual(code, 2)
        self.assertIn("--old-role and --new-role", err)

    def test_run_summaries_resolve_several_entries_of_one_id(self):
        old_root, new_root, dsl_b = self.caches()
        # The new cache holds a second beta entry (other field payloads, same DSL): ambiguous without the run.
        vm = "dslvm1_clang18.1_fma"
        other = -panel(NEW_SESSIONS, NEW_IDS)
        write_signal(new_root, f"{vm}/{self.new_sha}/fp_1111111111111111", "beta", dsl_b, self.new_sha, other)
        code, rep, err = run(["--kind", "signal", "--old", old_root, "--new", new_root, "--old-role", self.old_role,
                              "--new-role", self.new_role, "--out", self.out(), "--per-key"])
        self.assertEqual(code, 0, err)
        self.assertEqual(rep["unmatched"]["ambiguous"], ["beta"])
        self.assertFalse(rep["totals"]["bit_identical"])
        # Run summaries name the entries each run used: beta resolves to the identical one.
        for root, role_sha, name in ((old_root, self.old_sha, "run_old"), (new_root, self.new_sha, "run_new")):
            entries = []
            for sidecar in sorted(root.rglob("*.json")):
                j = json.loads(sidecar.read_text())
                if j.get("role_manifest_sha256") == role_sha and j.get("schema", "").startswith("atx.dsl-candidate-s"):
                    if j["candidate_id"] == "beta" and "fp_fedcba" in str(sidecar):
                        continue  # the new run used the fp_1111 entry
                    entries.append({"id": j["candidate_id"], "payload_sha256": j["payload_sha256"]})
            (self.tmp / name).mkdir()
            (self.tmp / name / "summary.json").write_text(json.dumps(
                {"roles": [{"role": "train", "manifest_sha256": role_sha,
                            "candidate_cache": {"entries": entries}}]}))
        code, rep, err = run(["--kind", "signal", "--old", old_root, "--new", new_root, "--old-role", self.old_role,
                              "--new-role", self.new_role, "--old-run", self.tmp / "run_old",
                              "--new-run", self.tmp / "run_new", "--out", self.out(), "--per-key"])
        self.assertEqual(code, 0, err)
        self.assertEqual(rep["unmatched"]["ambiguous"], [])
        rows = {r["key"]: r for r in rep["rows"]}
        self.assertIn("fp_1111111111111111", rows["beta"]["new_sidecar"])
        self.assertTrue(rep["totals"]["bit_identical"])

    def test_corrupt_payload_refused_unless_no_verify(self):
        old_root, new_root, _ = self.caches()
        payload = next(p for p in new_root.rglob("alpha.*.f64"))
        data = bytearray(payload.read_bytes())
        data[24] ^= 1  # cell (2021-01-04, id 20): finite and compared (columns 0 and 2 are new-only ids)
        payload.write_bytes(bytes(data))
        argv = ["--kind", "signal", "--old", old_root, "--new", new_root, "--old-role", self.old_role,
                "--new-role", self.new_role]
        code, _, err = run(argv + ["--out", self.out()])
        self.assertEqual(code, 2)
        self.assertIn("SHA-256 differs", err)
        code, rep, err = run(argv + ["--out", self.out(), "--no-verify"])
        self.assertEqual(code, 0, err)
        self.assertIn("alpha", rep["differing_keys"])


class DailyIcKind(Base):
    HEADER = ",".join(cwo.DAILY_IC_HEADER)

    def write_csv(self, path: Path, sessions, ids, change=None):
        lines = [self.HEADER]
        for cid in ids:
            for h in (5, 21, 63):
                for k, s in enumerate(sessions):
                    p = value(s, h + len(cid))
                    r = value(s, 2 * h + len(cid))
                    if change == (cid, h, s):
                        r += 1e-6
                    cells = ["" if math.isnan(v) else repr(v) for v in (p, r, -r)]
                    lines.append(f"{cid},{h},{400 + k},{ns(s)},{','.join(cells)}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + "\n")

    def test_aligned_by_id_and_date(self):
        old_dir, new_dir = self.tmp / "u_old", self.tmp / "u_new"
        self.write_csv(old_dir / cwo.DAILY_IC_FILE, OLD_SESSIONS, ["a", "bb"])
        # the new run has more sessions (and other decision indices) and one more id
        self.write_csv(new_dir / cwo.DAILY_IC_FILE, ["2020-12-31"] + NEW_SESSIONS, ["a", "bb", "ccc"],
                       change=("bb", 21, "2021-01-13"))
        code, rep, err = run(["--kind", "daily_ic", "--old", old_dir, "--new", new_dir, "--out", self.out(),
                              "--per-key"])
        self.assertEqual(code, 0, err)
        rows = {r["key"]: r for r in rep["rows"]}
        self.assertTrue(rows["a"]["bit_identical"])
        self.assertEqual(rows["a"]["cells_compared"], len(OLD_SESSIONS) * 3 * 3)
        self.assertEqual(rows["bb"]["unequal_cells"], 2)  # rank_ic and oriented_rank_ic
        self.assertEqual(rows["bb"]["first_diff"], {"session": "2021-01-13", "session_ns": ns("2021-01-13"),
                                                    "horizon": 21, "column": "rank_ic"})
        self.assertEqual(rep["new_only_keys"], ["ccc"])
        code, rep, err = run(["--kind", "daily_ic", "--old", old_dir / cwo.DAILY_IC_FILE, "--new", new_dir,
                              "--out", self.out(), "--before", "2021-01-13"])
        self.assertEqual(code, 0, err)
        self.assertTrue(rep["totals"]["bit_identical"])

    def test_sealed_row_refused(self):
        old_dir, new_dir = self.tmp / "s_old", self.tmp / "s_new"
        self.write_csv(old_dir / cwo.DAILY_IC_FILE, OLD_SESSIONS, ["a"])
        self.write_csv(new_dir / cwo.DAILY_IC_FILE, OLD_SESSIONS + ["2024-01-02"], ["a"])
        code, rep, err = run(["--kind", "daily_ic", "--old", old_dir, "--new", new_dir, "--out", self.out(),
                              "--before", "2022-09-30"])
        self.assertEqual(code, 2)
        self.assertIn("seal 2024-01-01", err)
        self.assertIsNone(rep)


class Seal(unittest.TestCase):
    def test_seal_is_2024_01_01_from_the_window(self):
        seal_ns, source = cwo.seal()
        self.assertEqual(seal_ns, ns("2024-01-01"))
        self.assertTrue("research_window" in source)


if __name__ == "__main__":
    unittest.main()
