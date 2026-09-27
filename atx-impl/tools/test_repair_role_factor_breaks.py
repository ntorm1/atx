"""Synthetic postimplementation checks for repair_role_factor_breaks (no real data).

Run: "C:/Program Files/Python312/python.exe" -m unittest discover -s atx-impl/tools -p test_repair_role_factor_breaks.py -v
"""
import contextlib
import csv
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
import repair_role_factor_breaks as rr  # noqa: E402

DAY = rr.DAY_NS
HOLIDAYS = {dt.date(2020, 11, 26), dt.date(2020, 12, 25), dt.date(2021, 1, 1), dt.date(2021, 1, 18)}
B1, B2 = dt.date(2021, 1, 4), dt.date(2021, 2, 1)

# name index -> role in the main scenario
DIVIDEND_LIKE = list(range(0, 60)) + list(range(65, 70))   # artifact decrease 1.5-10%, raw flat
OPPOSING = list(range(60, 65))                              # artifact -3% while raw +3%: not a jump cell
BIG_UP = list(range(70, 75))                                # later consolidation anchored at the break
BIG_DOWN = list(range(75, 78))                              # later forward split anchored at the break
SPLIT_ON_B, REVERSE_ON_B, DIVIDEND_ON_B = 78, 79, 80        # genuine same-day actions, no artifact
GAP_SHORT, GAP_LONG = 81, 82                                # absent on the break session (5 days / > 10 days)
SPLIT_NORMAL, DIVIDEND_NORMAL, HOLE = 83, 84, 85            # genuine actions / a one-session hole elsewhere
REPAIRED = DIVIDEND_LIKE + OPPOSING + BIG_UP + BIG_DOWN + [GAP_SHORT]


def sessions(start: str, count: int):
    d, out = dt.date.fromisoformat(start), []  # type: list[dt.date]
    while len(out) < count:
        if d.weekday() < 5 and d not in HOLIDAYS:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


def scenario(artifact=True, second=False, seed=7, cluster=None, partial=0):
    """raw (f32-widened), vendor factor, present; artifact factors k at B1 (and B2 when second)."""
    dates = sessions("2020-11-02", 80)
    D, N = len(dates), 100
    b, b2 = dates.index(B1), dates.index(B2)
    rng = np.random.default_rng(seed)
    ret = rng.normal(0.0, 0.01, (D, N))
    ret[0] = 0.0
    k = np.ones(N)
    k[DIVIDEND_LIKE] = np.linspace(0.90, 0.985, len(DIVIDEND_LIKE))
    k[OPPOSING] = 0.97
    k[BIG_UP] = [10.0, 25.0, 50.0, 80.0, 12.5]
    k[BIG_DOWN] = [0.25, 0.2, 0.5]
    k[[GAP_SHORT, GAP_LONG]] = 0.95
    ret[b, DIVIDEND_LIKE + BIG_UP + BIG_DOWN] = -0.002
    ret[b, OPPOSING] = 0.03
    ret[b, [SPLIT_ON_B, REVERSE_ON_B, DIVIDEND_ON_B]] = 0.0
    ret[20, DIVIDEND_NORMAL] = 0.03  # makes the normal-day dividend a (legitimate) jump cell
    if cluster is not None:  # a quarter-end ex-dividend cluster on an up day: names 0..59, 2% each, +3%
        ret[cluster, :60] = 0.03
    k2 = np.ones(N)
    if second:  # a second re-anchoring on B2 for names 0..59, and one on name 90 (absent across both)
        k2[:60] = np.linspace(0.95, 0.985, 60)
        k2[90] = 0.97
        ret[b2, :60] = -0.002
    raw = 20.0 * np.exp(np.cumsum(ret, axis=0)) * (1.0 + np.arange(N) / N)
    factor = np.ones((D, N))
    if not artifact:
        k[:] = 1.0
    if partial:  # a partial re-anchoring of the first `partial` names only
        k[:partial] = np.linspace(0.95, 0.99, partial)
    factor[b:] *= k  # the vendor's pre-break block is anchored at 1 (f1prev = 1 on the break)
    factor[b2:] *= k2
    # genuine actions: raw moves against the factor, adjusted close continuous
    raw[b:, SPLIT_ON_B] *= 0.5
    factor[:b, SPLIT_ON_B] *= 0.5
    raw[b:, REVERSE_ON_B] *= 10.0
    factor[:b, REVERSE_ON_B] *= 10.0
    raw[b:, DIVIDEND_ON_B] *= 0.98
    factor[:b, DIVIDEND_ON_B] *= 0.98
    raw[10:, SPLIT_NORMAL] *= 0.5
    factor[:10, SPLIT_NORMAL] *= 0.5
    raw[20:, DIVIDEND_NORMAL] *= 0.98
    factor[:20, DIVIDEND_NORMAL] *= 0.98
    if cluster is not None:
        raw[cluster:, :60] *= 0.98
        factor[:cluster, :60] *= 0.98
    raw = raw.astype(np.float32).astype(np.float64)
    present = np.ones((D, N), dtype=bool)
    present[b, GAP_SHORT] = False
    present[b - 10:b + 3, GAP_LONG] = False
    present[30, HOLE] = False
    if second:
        present[b - 1:b2 + 1, 90] = False
    return {"dates": dates, "raw": raw, "factor": factor, "present": present, "k": k, "k2": k2, "b": b, "b2": b2}


def write_role(directory: Path, sc) -> str:
    directory.mkdir()
    dates, present = sc["dates"], sc["present"]
    D, N = present.shape
    raw = np.where(present, sc["raw"], np.nan)
    close = np.where(present, sc["raw"] * sc["factor"], np.nan)
    volume = np.where(present, 1e6 + np.arange(N, dtype=np.float64), np.nan)
    member = (present & (np.arange(D)[:, None] >= 1)).astype("u1")
    days = np.array([(d - dt.date(1970, 1, 1)).days for d in dates], dtype=np.int64)
    payload = {"sessions.i64": (days * DAY).astype("<i8"), "ids.u64": (1000 + 7 * np.arange(N)).astype("<u8"),
               "close.f64": close.astype("<f8"), "raw_close.f64": raw.astype("<f8"),
               "volume.f64": volume.astype("<f8"), "present.u8": present.astype("u1"), "member.u8": member}
    files = {}
    for name, value in payload.items():
        blob = value.tobytes()
        (directory / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": D, "instruments": N,
                "instrument_namespace": "spiderrock.securityID", "score_begin": 5, "score_end": D,
                "score_start_ns": int(days[5]) * DAY, "score_end_ns": int(days[-1] + 1) * DAY,
                "warmup_start": dates[0].isoformat(), "source_sha256": "0" * 64,
                "projection_manifest_sha256": "1" * 64, "membership_recipe": "{\"rule\":\"x\"}",
                "clock_recipe": "modeled-session+22h-mark+23h-decision-v1",
                "close_basis": "f64(raw-f32-close)*f64-cumulReturnFactor", "volume_basis": "raw-share-volume",
                "common_stock_verified": False, "historical_vintage_verified": False,
                "declared_output_bytes": D * N * 26 + D * 8 + N * 8,
                "score_member_counts": [int(x) for x in member.sum(axis=1)[5:]], "files": files}
    text = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
    (directory / "manifest.json").write_bytes(text)
    return hashlib.sha256(text).hexdigest()


def run(*args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = rr.main([str(a) for a in args])
    return code, out.getvalue(), err.getvalue()


def load(directory: Path, name: str, shape, dtype="<f8"):
    return np.fromfile(directory / name, dtype=dtype).reshape(shape)


class RepairFixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.sc = scenario()
        self.role = self.base / "role"
        self.sha = write_role(self.role, self.sc)
        self.shape = self.sc["present"].shape

    def tearDown(self):
        self.tmp.cleanup()

    def repair(self, name="out", *extra):
        out = self.base / name
        code, text, err = run("--role", self.role, "--role-sha256", self.sha, "--out", out, *extra)
        self.assertEqual(code, 0, err + text)
        return out, text

    def test_mass_break_repaired_exactly(self):
        out, text = self.repair("out", "--expect-sessions", "2021-01-04")
        self.assertIn("verdict factor-break-v1: MASS 1 session(s): 2021-01-04", text)
        self.assertIn("verdict factor-break-v2: MASS 1 session(s): 2021-01-04", text)
        old = load(self.role, "close.f64", self.shape)
        new = load(out, "close.f64", self.shape)
        raw, present, k, b = self.sc["raw"], self.sc["present"], self.sc["k"], self.sc["b"]
        for j in REPAIRED:
            t = b + 1 if j == GAP_SHORT else b
            p = b - 1
            on = present[:, j]
            # the repaired history is the continuously anchored one: close' = raw * k everywhere
            np.testing.assert_allclose(new[on, j] / raw[on, j], k[j], rtol=2e-15, atol=0)
            self.assertEqual(new[t:, j].tobytes(), old[t:, j].tobytes())  # vendor anchoring kept after the break
            adj, rawr = math.log(new[t, j] / new[p, j]), math.log(raw[t, j] / raw[p, j])
            self.assertLess(abs(adj - rawr), 1e-12, j)
        untouched = sorted(set(range(self.shape[1])) - set(REPAIRED))
        self.assertEqual(new[:, untouched].tobytes(), old[:, untouched].tobytes())
        m = json.loads((out / "manifest.json").read_bytes())
        rep = m["repair"]
        self.assertEqual(rep["rule"], "factor-break-v1")
        self.assertEqual(rep["source_role"]["manifest_sha256"], self.sha)
        s = rep["mass_sessions"]
        self.assertEqual([x["session"] for x in s], ["2021-01-04"])
        self.assertEqual(s[0]["repaired"], len(REPAIRED))
        self.assertEqual((s[0]["kept_split_follow"], s[0]["kept_distribution"], s[0]["kept_gap"]), (2, 1, 1))
        self.assertEqual(s[0]["jump_cells"], len(DIVIDEND_LIKE) + len(BIG_UP) + len(BIG_DOWN))
        self.assertLess(s[0]["post_repair_jump_cells"], rr.MASS_MIN_CELLS)
        self.assertEqual(rep["repaired_names"], len(REPAIRED))
        self.assertLessEqual(rep["max_repaired_return_error_ln"], 1e-12)

    def test_genuine_split_and_dividend_untouched(self):
        out, text = self.repair()
        old = load(self.role, "close.f64", self.shape)
        new = load(out, "close.f64", self.shape)
        for j in (SPLIT_ON_B, REVERSE_ON_B, DIVIDEND_ON_B, SPLIT_NORMAL, DIVIDEND_NORMAL, GAP_LONG, HOLE):
            self.assertEqual(new[:, j].tobytes(), old[:, j].tobytes(), j)
        with (out / rr.CELLS_FILE).open(newline="") as f:
            rows = {int(r["column"]): r for r in csv.DictReader(f)}
        self.assertEqual(rows[SPLIT_ON_B]["action"], "kept_split_follow")
        self.assertEqual(rows[REVERSE_ON_B]["action"], "kept_split_follow")
        self.assertEqual(rows[DIVIDEND_ON_B]["action"], "kept_distribution")
        self.assertEqual(rows[GAP_LONG]["action"], "kept_gap")
        self.assertEqual(rows[GAP_SHORT]["action"], "repaired")
        self.assertEqual(rows[GAP_SHORT]["prev_session"], "2020-12-31")
        self.assertEqual(rows[GAP_SHORT]["session"], "2021-01-05")
        self.assertNotIn(SPLIT_NORMAL, rows)  # normal-day actions are never listed
        self.assertNotIn(DIVIDEND_NORMAL, rows)
        self.assertEqual(sorted(j for j, r in rows.items() if r["action"] == "repaired"), sorted(REPAIRED))

    def test_other_files_and_manifest_schema_preserved(self):
        out, _ = self.repair()
        src = json.loads((self.role / "manifest.json").read_bytes())
        new = json.loads((out / "manifest.json").read_bytes())
        self.assertEqual(set(new), set(src) | {"repair"})
        for key in src:
            if key != "files":
                self.assertEqual(new[key], src[key], key)
        for name, entry in src["files"].items():
            if name == "close.f64":
                self.assertEqual(new["files"][name]["bytes"], entry["bytes"])
                self.assertNotEqual(new["files"][name]["sha256"], entry["sha256"])
            else:
                self.assertEqual(new["files"][name], entry)
                self.assertEqual((out / name).read_bytes(), (self.role / name).read_bytes())
        cells = (out / rr.CELLS_FILE).read_bytes()
        self.assertEqual(new["repair"]["cells"]["sha256"], hashlib.sha256(cells).hexdigest())
        blob = (Path(rr.__file__).read_bytes()).replace(b"\r\n", b"\n")
        self.assertEqual(new["repair"]["tool"]["code_git_blob_sha1"],
                         hashlib.sha1(b"blob %d\0" % len(blob) + blob).hexdigest())
        # the repaired role satisfies the loader contract and binds by its own manifest SHA
        sha = hashlib.sha256((out / "manifest.json").read_bytes()).hexdigest()
        again = rr.load_role(out, sha, tuple(rr.DTYPES))
        self.assertEqual(again["manifest"]["close_basis"], "f64(raw-f32-close)*f64-cumulReturnFactor")
        self.assertEqual(sorted(p.name for p in out.iterdir()), sorted([*rr.DTYPES, "manifest.json", rr.CELLS_FILE]))

    def test_output_bytes_deterministic(self):
        a, _ = self.repair("a")
        b, _ = self.repair("b")
        names = sorted(p.name for p in a.iterdir())
        self.assertEqual(names, sorted(p.name for p in b.iterdir()))
        for name in names:
            self.assertEqual((a / name).read_bytes(), (b / name).read_bytes(), name)

    def test_exclusive_output(self):
        out = self.base / "taken"
        out.mkdir()
        (out / "keep.txt").write_text("mine")
        code, _, err = run("--role", self.role, "--role-sha256", self.sha, "--out", out)
        self.assertEqual(code, rr.EXIT_REFUSED)
        self.assertIn("exclusive", err)
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["keep.txt"])
        code, _, _ = run("--role", self.role, "--role-sha256", self.sha, "--out", self.base / "no" / "parent")
        self.assertEqual(code, rr.EXIT_REFUSED)
        self.assertFalse((self.base / "no").exists())

    def test_sha_refusal(self):
        code, _, err = run("--role", self.role, "--role-sha256", "f" * 64, "--out", self.base / "x")
        self.assertEqual(code, rr.EXIT_REFUSED)
        self.assertIn("does not match --role-sha256", err)
        self.assertFalse((self.base / "x").exists())
        code, _, err = run("--role", self.role, "--role-sha256", "f" * 64, "--scan-only")
        self.assertEqual(code, rr.EXIT_REFUSED)
        blob = bytearray((self.role / "close.f64").read_bytes())
        blob[8] ^= 1
        (self.role / "close.f64").write_bytes(bytes(blob))
        code, _, err = run("--role", self.role, "--role-sha256", self.sha, "--out", self.base / "y")
        self.assertEqual(code, rr.EXIT_REFUSED)
        self.assertIn("close.f64 bytes do not match", err)
        self.assertFalse((self.base / "y").exists())

    def test_scan_only_writes_nothing_and_matches_nav_recon_columns(self):
        before = sorted(p.name for p in self.base.rglob("*"))
        code, text, err = run("--role", self.role, "--role-sha256", self.sha, "--scan-only")
        self.assertEqual(code, 0, err)
        self.assertEqual(sorted(p.name for p in self.base.rglob("*")), before)
        self.assertIn("2021-01-04", text)
        self.assertIn("MASS", text)
        self.assertIn(f"repaired {len(REPAIRED)},", text)
        csv_path = self.base / "scan.csv"
        code, _, _ = run("--role", self.role, "--role-sha256", self.sha, "--scan-only", "--csv", csv_path)
        self.assertEqual(code, 0)
        with csv_path.open(newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(len(rows), self.shape[0])
        close = np.where(self.sc["present"], self.sc["raw"] * self.sc["factor"], np.nan)
        raw = np.where(self.sc["present"], self.sc["raw"], np.nan)
        # literal nav_recon section E (adjacent present sessions only)
        for t in range(1, self.shape[0]):
            big = small = 0
            for i in range(self.shape[1]):
                if not (self.sc["present"][t, i] and self.sc["present"][t - 1, i]):
                    continue
                al = math.log(close[t, i]) - math.log(close[t - 1, i])
                rl = math.log(raw[t, i]) - math.log(raw[t - 1, i])
                dlf = al - rl
                big += abs(dlf) > 0.10 and abs(al) > abs(rl) + 0.10
                small += 0.01 < abs(dlf) <= 0.10 and abs(al) > abs(rl) + 0.01
            self.assertEqual((int(rows[t]["big"]), int(rows[t]["small"])), (big, small), rows[t]["session"])
        self.assertEqual([r["session"] for r in rows if r["mass_v1"] == "1"], ["2021-01-04"])
        self.assertEqual([r["session"] for r in rows if r["mass_v2"] == "1"], ["2021-01-04"])
        # every repaired step ending on the session is unexplained; the gap-spanning one ends a session later
        self.assertEqual(int(rows[self.sc["b"]]["unexplained"]), len(REPAIRED) - 1)
        self.assertEqual(int(rows[self.sc["b"] + 1]["unexplained"]), 1)
        self.assertEqual(sum(int(r["unexplained"]) for r in rows), len(REPAIRED))
        f1p = int(rows[self.sc["b"]]["f1_prev"]) / int(rows[self.sc["b"]]["steps"])
        self.assertGreater(f1p, 0.9)  # the pre-break block is anchored at 1
        code, _, err = run("--role", self.role, "--role-sha256", self.sha, "--scan-only", "--csv", csv_path)
        self.assertEqual(code, rr.EXIT_REFUSED)  # exclusive

    def test_expect_sessions_mismatch_refuses(self):
        code, _, err = run("--role", self.role, "--role-sha256", self.sha, "--out", self.base / "z",
                           "--expect-sessions", "none")
        self.assertEqual(code, rr.EXIT_REFUSED)
        self.assertIn("differ from --expect-sessions", err)
        self.assertFalse((self.base / "z").exists())


class CleanAndMultiSession(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_no_mass_session(self):
        sc = scenario(artifact=False)
        role = self.base / "role"
        sha = write_role(role, sc)
        code, text, _ = run("--role", role, "--role-sha256", sha, "--scan-only")
        self.assertEqual(code, 0)
        self.assertIn("verdict factor-break-v1: CLEAN", text)
        self.assertIn("verdict factor-break-v2: CLEAN", text)
        code, text, _ = run("--role", role, "--role-sha256", sha, "--out", self.base / "out")
        self.assertEqual(code, rr.EXIT_CLEAN)
        self.assertFalse((self.base / "out").exists())
        code, text, err = run("--role", role, "--role-sha256", sha, "--out", self.base / "noop", "--allow-noop",
                              "--expect-sessions", "none")
        self.assertEqual(code, 0, err)
        self.assertEqual((self.base / "noop" / "close.f64").read_bytes(), (role / "close.f64").read_bytes())
        m = json.loads((self.base / "noop" / "manifest.json").read_bytes())
        self.assertTrue(m["repair"]["noop"])
        self.assertEqual(m["repair"]["mass_sessions"], [])
        self.assertEqual(m["files"], json.loads((role / "manifest.json").read_bytes())["files"])

    def test_two_mass_sessions_compound(self):
        sc = scenario(second=True)
        role = self.base / "role"
        sha = write_role(role, sc)
        code, text, err = run("--role", role, "--role-sha256", sha, "--out", self.base / "out",
                              "--expect-sessions", "2021-02-01,2021-01-04")
        self.assertEqual(code, 0, err + text)
        shape = sc["present"].shape
        new = load(self.base / "out", "close.f64", shape)
        raw, present = sc["raw"], sc["present"]
        for j in range(60):
            on = present[:, j]
            np.testing.assert_allclose(new[on, j] / raw[on, j], sc["k"][j] * sc["k2"][j], rtol=4e-15, atol=0)
        with (self.base / "out" / rr.CELLS_FILE).open(newline="") as f:
            rows = [r for r in csv.DictReader(f) if r["column"] == "90"]
        self.assertEqual(len(rows), 1)  # one step across both sessions: listed once
        self.assertEqual(rows[0]["action"], "kept_gap")


V1_COMMIT = "99421a5f"  # the tool that built TRAIN role v2 (factor-break-v1)


class RuleV2(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def role(self, sc, name="role"):
        path = self.base / name
        return path, write_role(path, sc)

    def test_dividend_cluster_is_not_a_v2_break(self):
        """A normal quarter-end ex-dividend cluster of 60 names on an up day: v1 false MASS, v2 CLEAN."""
        sc = scenario(artifact=False, cluster=35)
        role, sha = self.role(sc)
        day = sc["dates"][35].isoformat()
        code, text, err = run("--role", role, "--role-sha256", sha, "--scan-only", "--csv", self.base / "s.csv")
        self.assertEqual(code, 0, err)
        self.assertIn(f"verdict factor-break-v1: MASS 1 session(s): {day}", text)
        self.assertIn("verdict factor-break-v2: CLEAN", text)
        with (self.base / "s.csv").open(newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertGreaterEqual(int(rows[35]["jump"]), 60)
        self.assertEqual(sum(int(r["unexplained"]) for r in rows), 0)
        # v2: nothing to repair; v1: every cluster step is kept_distribution, so its post-repair gate refuses
        code, _, _ = run("--role", role, "--role-sha256", sha, "--rule", "v2", "--out", self.base / "v2")
        self.assertEqual(code, rr.EXIT_CLEAN)
        self.assertFalse((self.base / "v2").exists())
        code, _, err = run("--role", role, "--role-sha256", sha, "--rule", "v1", "--out", self.base / "v1")
        self.assertEqual(code, rr.EXIT_REFUSED)
        self.assertIn("a mass session remains after repair", err)
        self.assertFalse((self.base / "v1").exists())

    def test_small_unexplained_break_is_a_v2_mass(self):
        """A 30-name re-anchoring (below v1's 50 jump cells) is a v2 MASS and is repaired exactly."""
        sc = scenario(artifact=False, partial=30)
        role, sha = self.role(sc)
        code, text, err = run("--role", role, "--role-sha256", sha, "--scan-only")
        self.assertEqual(code, 0, err)
        self.assertIn("verdict factor-break-v1: CLEAN", text)
        self.assertIn("verdict factor-break-v2: MASS 1 session(s): 2021-01-04", text)
        code, _, _ = run("--role", role, "--role-sha256", sha, "--out", self.base / "v1")  # default v1
        self.assertEqual(code, rr.EXIT_CLEAN)
        out = self.base / "v2"
        code, text, err = run("--role", role, "--role-sha256", sha, "--rule", "v2", "--out", out,
                              "--expect-sessions", "2021-01-04")
        self.assertEqual(code, 0, err + text)
        shape = sc["present"].shape
        old, new = load(role, "close.f64", shape), load(out, "close.f64", shape)
        for j in range(30):
            np.testing.assert_allclose(new[:, j] / sc["raw"][:, j], sc["k"][j], rtol=2e-15, atol=0)
        self.assertEqual(new[:, 30:].tobytes(), old[:, 30:].tobytes())
        rep = json.loads((out / "manifest.json").read_bytes())["repair"]
        self.assertEqual(rep["rule"], "factor-break-v2")
        self.assertEqual(rep["parameters"], rr.PARAMETERS_V2)
        s = rep["mass_sessions"][0]
        self.assertEqual((s["session"], s["unexplained_steps"], s["unexplained_threshold"], s["repaired"],
                          s["post_repair_unexplained_steps"]), ("2021-01-04", 30, 20, 30, 0))
        self.assertEqual(sum(rep["detector_unexplained_steps_by_session"]), 30)

    def test_v2_threshold_is_a_share_of_steps(self):
        steps = np.array([0, 100, 1999, 2000, 2001, 5000])
        np.testing.assert_array_equal(rr.unexplained_threshold(steps), [20, 20, 20, 20, 21, 50])

    def test_v1_and_v2_agree_on_the_main_scenario(self):
        role, sha = self.role(scenario())
        outs = {}
        for rule in ("v1", "v2"):
            outs[rule] = self.base / rule
            code, _, err = run("--role", role, "--role-sha256", sha, "--rule", rule, "--out", outs[rule],
                               "--expect-sessions", "2021-01-04")
            self.assertEqual(code, 0, err)
        for name in (*rr.DTYPES, rr.CELLS_FILE):
            self.assertEqual((outs["v1"] / name).read_bytes(), (outs["v2"] / name).read_bytes(), name)
        m1 = json.loads((outs["v1"] / "manifest.json").read_bytes())["repair"]
        m2 = json.loads((outs["v2"] / "manifest.json").read_bytes())["repair"]
        self.assertEqual((m1["rule"], m2["rule"]), ("factor-break-v1", "factor-break-v2"))
        self.assertNotIn("detector_unexplained_steps_by_session", m1)
        self.assertNotIn("unexplained_steps", m1["mass_sessions"][0])
        self.assertEqual(m2["mass_sessions"][0]["unexplained_steps"], len(REPAIRED) - 1)

    def test_v1_reproduces_the_committed_tool(self):
        """--rule v1 (the default) writes the same bytes as the tool that built TRAIN role v2, except
        the manifest's repair.tool identity (the code itself changed)."""
        import importlib.util
        import subprocess
        root = Path(__file__).resolve().parents[2]
        try:
            code = subprocess.run(["git", "-C", str(root), "show", f"{V1_COMMIT}:atx-impl/tools/repair_role_factor_breaks.py"],
                                  capture_output=True, check=True, timeout=60).stdout
        except (OSError, subprocess.SubprocessError):
            self.skipTest(f"git or commit {V1_COMMIT} unavailable")
        path = self.base / "repair_role_factor_breaks_v1_committed.py"
        path.write_bytes(code)
        spec = importlib.util.spec_from_file_location("repair_role_factor_breaks_v1_committed", path)
        old_tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(old_tool)
        role, sha = self.role(scenario(second=True))
        args = ["--role", str(role), "--role-sha256", sha, "--expect-sessions", "2021-01-04,2021-02-01"]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(old_tool.main(args + ["--out", str(self.base / "old")]), 0)
        code, _, err = run(*args, "--out", self.base / "new")
        self.assertEqual(code, 0, err)
        for name in (*rr.DTYPES, rr.CELLS_FILE):
            self.assertEqual((self.base / "old" / name).read_bytes(), (self.base / "new" / name).read_bytes(), name)
        old_m = json.loads((self.base / "old" / "manifest.json").read_bytes())
        new_m = json.loads((self.base / "new" / "manifest.json").read_bytes())
        self.assertNotEqual(old_m["repair"].pop("tool"), new_m["repair"].pop("tool"))
        self.assertEqual(old_m, new_m)

    def test_malformed_manifest_is_refused_not_a_traceback(self):
        role, _ = self.role(scenario())
        m = json.loads((role / "manifest.json").read_bytes())
        m["dates"] = None
        text = (json.dumps(m, sort_keys=True, indent=2) + "\n").encode()
        (role / "manifest.json").write_bytes(text)
        code, _, err = run("--role", role, "--role-sha256", hashlib.sha256(text).hexdigest(), "--scan-only")
        self.assertEqual(code, rr.EXIT_REFUSED)
        self.assertIn("TypeError", err)


if __name__ == "__main__":
    unittest.main()
