"""research_fields_ivshape (platform v8 lane YDATA): synthetic vendor source and role only, no real archive access.

The world: a TickerHistory3-like parquet (dates mixed in every row group) from 2018-06 to 2019-03 for five role lines,
the market line SPY (not a role line) and one line off the role axis, with a duplicated key, an out-of-domain IV, an
exact-zero slope, a slope above the bound, a null slope, a six-session hole in SPY's history, a Saturday row and rows
after the seal. The role is the NYSE sessions 2018-07-02 .. 2019-02-28 projected from it. Both fields are built through
the builder's run() with the draft module registered, checked cell by cell against definitions written out here
independently, and put through a look-ahead probe shown to fail on a leaky variant of the module."""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest
import unittest.mock as mock

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_research_fields as tool
import prepare_research_fields_ydata as yreg
import research_fields_ivshape as ivs
import research_fields_sec as sec
import research_window as rw

EPOCH = dt.date(1970, 1, 1)
DAY_NS = 86_400_000_000_000
TH_FIRST, ROLE_FIRST, ROLE_LAST, TH_LAST = dt.date(2018, 6, 1), dt.date(2018, 7, 2), dt.date(2019, 2, 28), \
    dt.date(2019, 3, 29)
ROLE_IDS = [11, 22, 33, 44, 55]
SPY, OFF_ROLE = 77, 99
TICKER = {11: "AAA", 22: "BBB", 33: "CCC", 44: "DDD", 55: "EEE", SPY: "SPY", OFF_ROLE: "ZZZ"}
FIELDS = list(yreg.FIELDS_IVSHAPE_DRAFT)
DUP_KEY = (33, dt.date(2018, 8, 1))
IV_OUT = (44, dt.date(2018, 11, 5))             # IV 7.0: outside the domain (no surface)
ZERO_SLOPE = (55, dt.date(2018, 8, 20))         # an exact 0.0 slope: the vendor's empty print
BIG_SLOPE = (11, dt.date(2019, 1, 9))           # |slope| above 5
NULL_SLOPE = (22, dt.date(2018, 9, 12))
SPY_HOLE = (dt.date(2018, 10, 1), dt.date(2018, 10, 8))   # six SPY sessions missing
EXTRA = [(dt.date(2018, 9, 8), 11),            # a Saturday: off the calendar
         (dt.date(2025, 2, 3), 22), (dt.date(2024, 3, 1), 11), (dt.date(2024, 3, 1), SPY)]   # on or after the seal
CUT = 90                                        # the probe's first mutated role row


def day(d):
    return (d - EPOCH).days


def sessions(first, last):
    return [EPOCH + dt.timedelta(days=int(x)) for x in sec.nyse_sessions(first, last)]


COLUMNS = ("tradingDate", "securityID", "ticker_tk", "close", "cumulReturnFactor", "volume", "shD1", "atmCenI_21d")
TYPES = {"tradingDate": pa.date32(), "securityID": pa.int64(), "ticker_tk": pa.string(), "close": pa.float32(),
         "cumulReturnFactor": pa.float64(), "volume": pa.float64(), "shD1": pa.float32(), "atmCenI_21d": pa.float32()}


def world(seed=20261002, flip=1.0):
    """Vendor rows {column: list} (None = a null cell). ``flip`` multiplies every slope (the other sign convention)."""
    rng = np.random.default_rng(seed)
    cal = sessions(TH_FIRST, TH_LAST)
    rows = {k: [] for k in COLUMNS}
    for n, sid in enumerate(ROLE_IDS + [SPY, OFF_ROLE]):
        c, iv = 20.0 + 13.0 * n, 0.2 + 0.05 * n
        for d in cal:
            if sid == SPY and SPY_HOLE[0] <= d <= SPY_HOLE[1]:
                continue
            c *= math.exp(rng.normal(0.0, 0.015))
            iv = min(max(iv * math.exp(rng.normal(0.0, 0.05)), 0.05), 2.0)
            slope = (-0.08 + rng.normal(0.0, 0.01)) if sid == SPY else rng.normal(-0.04, 0.05)
            if (sid, d) == ZERO_SLOPE:
                slope = 0.0
            if (sid, d) == BIG_SLOPE:
                slope = 6.0
            row = {"tradingDate": d, "securityID": sid, "ticker_tk": TICKER[sid], "close": c, "cumulReturnFactor": 1.0,
                   "volume": float(rng.integers(10_000, 90_000)),
                   "shD1": None if (sid, d) == NULL_SLOPE else slope * flip,
                   "atmCenI_21d": 7.0 if (sid, d) == IV_OUT else iv}
            for k, v in row.items():
                rows[k].append(v)
            if (sid, d) == DUP_KEY:
                for k, v in row.items():
                    rows[k].append(v * 1.1 if k in ("shD1", "atmCenI_21d") else v)
    for d, sid in EXTRA:
        for k, v in (("tradingDate", d), ("securityID", sid), ("ticker_tk", TICKER[sid]), ("close", 50.0),
                     ("cumulReturnFactor", 1.0), ("volume", 1e4), ("shD1", -0.3 * flip), ("atmCenI_21d", 0.4)):
            rows[k].append(v)
    return rows


def write_source(path, rows, drop=()):
    order = np.random.default_rng(7).permutation(len(rows["securityID"]))     # dates mixed in every row group
    pq.write_table(pa.table({k: pa.array([rows[k][i] for i in order], t) for k, t in TYPES.items() if k not in drop}),
                   path, row_group_size=300)
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def f32(x):
    return math.nan if x is None else float(np.float32(x))


def keyed(rows):
    """{(sid, epoch day): row index} of the unique keys."""
    seen, keep = {}, {}
    for i, (d, s) in enumerate(zip(rows["tradingDate"], rows["securityID"])):
        seen[(s, day(d))] = seen.get((s, day(d)), 0) + 1
        keep[(s, day(d))] = i
    return {k: i for k, i in keep.items() if seen[k] == 1}


def write_role(root, rows, source_sha):
    """The role projection's contract: close = f64(f32 raw close) x factor on unique valid keys."""
    root.mkdir(parents=True)
    keep = keyed(rows)
    days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]
    nd, n = len(days), len(ROLE_IDS)
    close, raw, volume = (np.full((nd, n), np.nan) for _ in range(3))
    present = np.zeros((nd, n), dtype="u1")
    for t, d in enumerate(days):
        for j, sid in enumerate(ROLE_IDS):
            i = keep.get((sid, d))
            if i is None:
                continue
            r, f, v = f32(rows["close"][i]), rows["cumulReturnFactor"][i], rows["volume"][i]
            raw[t, j], close[t, j], volume[t, j], present[t, j] = r, r * f, v, 1
    member = present.copy()
    member[5:9, 0] = 0
    blobs = {"sessions.i64": np.array([d * DAY_NS for d in days], dtype="<i8").tobytes(),
             "ids.u64": np.array(ROLE_IDS, dtype="<u8").tobytes(), "member.u8": member.tobytes(),
             "close.f64": close.astype("<f8").tobytes(), "raw_close.f64": raw.astype("<f8").tobytes(),
             "present.u8": present.tobytes(), "volume.f64": volume.astype("<f8").tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": nd, "instruments": n,
                "instrument_namespace": "spiderrock.securityID", "score_begin": 20, "score_end": nd,
                "source_sha256": source_sha, "files": files, "clock_recipe": "modeled-session+22h-mark+23h-decision-v1"}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return hashlib.sha256((root / "manifest.json").read_bytes()).hexdigest()


@contextlib.contextmanager
def registered():
    """The draft modules registered into the builder for the duration (undone on exit)."""
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        yreg.register(vars(tool))
        yield


class Case:
    def __init__(self, base: Path, rows, name="w", drop=()):
        self.base, self.rows = base, rows
        self.src = base / f"{name}.parquet"
        sha = write_source(self.src, rows, drop)
        self.role = base / f"{name}-role"
        self.role_sha = write_role(self.role, rows, sha)
        self.days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]

    def run(self, out, fields, **kw):
        kw.setdefault("module_options", {"price_source": self.src})
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(self.role, self.role_sha, self.base / out, fields, **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(self.days), len(ROLE_IDS))


def valid_iv(v):
    return math.isfinite(v) and np.float32(0.02) <= np.float32(v) <= np.float32(5.0)


def reference(rows, days):
    """The field definitions written out independently: {name: (dates x lines)}."""
    keep = keyed(rows)

    def cell(sid, d):
        i = keep.get((sid, d))
        return (math.nan, math.nan) if i is None else (f32(rows["shD1"][i]), f32(rows["atmCenI_21d"][i]))

    def slope(sid, d):
        s, v = cell(sid, d)
        return s if valid_iv(v) and math.isfinite(s) and s != 0 and abs(s) <= 5.0 else math.nan

    nd = len(days)
    out = {x: np.full((nd, len(ROLE_IDS)), np.nan) for x in FIELDS}
    for t in range(nd):
        if t < 21:
            continue
        window = days[t - 21:t]
        spy = [slope(SPY, d) for d in window]
        spy = [x for x in spy if math.isfinite(x)]
        o = math.nan
        if len(spy) >= 17:
            m = float(np.median(spy))
            o = math.copysign(1.0, m) if m != 0 else math.nan
        for j, sid in enumerate(ROLE_IDS):
            out["iv_skew_21"][t, j] = o * slope(sid, days[t - 1])
            xs = [cell(sid, d)[1] for d in window]
            xs = [x for x in xs if valid_iv(x)]
            if len(xs) >= 17:
                out["iv_vov_21"][t, j] = float(np.std(xs, ddof=1) / np.mean(xs))
    return out


def mutated(rows, first_day, seed=5):
    """Every vendor row dated on or after ``first_day`` (every line, SPY included) moved at random."""
    rng = np.random.default_rng(seed)
    late = {k: list(v) for k, v in rows.items()}
    for i, d in enumerate(late["tradingDate"]):
        if day(d) >= first_day:
            if late["shD1"][i] is not None:
                late["shD1"][i] *= float(rng.uniform(-1.5, 1.5))
            late["atmCenI_21d"][i] = min(late["atmCenI_21d"][i] * float(rng.uniform(0.7, 1.3)), 4.0)
    return late


class IvShapeFields(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self._reg = registered()
        self._reg.__enter__()

    def tearDown(self):
        self._reg.__exit__(None, None, None)
        self._tmp.cleanup()

    def probe(self, name, cut_row, patches=()):
        """The look-ahead probe: build ``name`` on the world and on the world with every row dated on or after
        session ``cut_row`` moved; returns (rows <= cut_row bit-identical, rows after cut_row differ)."""
        rows = world()
        days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]
        with contextlib.ExitStack() as stack:
            for target, attr, value in patches:
                stack.enter_context(mock.patch.object(target, attr, value))
            tag = f"{name}-{len(list(self.base.iterdir()))}"
            a = Case(self.base, rows, tag + "a")
            a.run(tag + "fa", [name])
            b = Case(self.base, mutated(rows, days[cut_row]), tag + "b")
            b.run(tag + "fb", [name])
        x, y = a.field(tag + "fa", name), b.field(tag + "fb", name)
        return x[:cut_row + 1].tobytes() == y[:cut_row + 1].tobytes(), x[cut_row + 1:].tobytes() != y[cut_row + 1:].tobytes()

    def test_values_match_definitions(self):
        rows = world()
        case = Case(self.base, rows)
        m = case.run("f", FIELDS)
        ref = reference(rows, case.days)
        for name in FIELDS:
            got = case.field("f", name)
            self.assertTrue(np.array_equal(np.isnan(got), np.isnan(ref[name])), name)
            ok = np.isfinite(ref[name])
            np.testing.assert_allclose(got[ok], ref[name][ok], rtol=1e-12, atol=0, err_msg=name)
            self.assertGreater(int(ok.sum()), 0.6 * got.size, name)
        t_of = {d: t for t, d in enumerate(case.days)}
        col = {sid: j for j, sid in enumerate(ROLE_IDS)}
        skew = case.field("f", "iv_skew_21")
        for sid, d in (DUP_KEY, IV_OUT, ZERO_SLOPE, BIG_SLOPE, NULL_SLOPE):     # read at the next session
            self.assertTrue(math.isnan(skew[t_of[day(d)] + 1, col[sid]]), (sid, d))
        # SPY's slopes are negative in this world: orientation -1, so the value is minus the vendor slope
        t = t_of[day(dt.date(2018, 12, 3))]
        keep = keyed(rows)
        s = f32(rows["shD1"][keep[(22, case.days[t - 1])]])
        self.assertEqual(skew[t, col[22]], -s)
        # the SPY hole: a window holding 6 missing sessions has 15 < 17 SPY slopes -> the whole row is NaN
        hole_end = t_of[day(SPY_HOLE[1])]
        self.assertTrue(np.all(np.isnan(skew[hole_end + 1])))
        e = {x["name"]: x for x in m["fields"]}
        for name in FIELDS:
            self.assertTrue(e[name]["point_in_time"])
            self.assertEqual(e[name]["producer"]["module"], "research_fields_ivshape.py")
            self.assertEqual(e[name]["lag_sessions"], 1)
            self.assertEqual(e[name]["formula_id"], ivs.FIELDS[name]["formula_id"])
            self.assertEqual(e[name]["formula_sha256"], tool.formula_id(name, tool.spec_definition(name, 1)))
        self.assertGreater(e["iv_skew_21"]["sessions_unoriented"], 21)          # warm-up and the SPY hole
        self.assertEqual(e["iv_skew_21"]["sessions_oriented_positive"], 0)
        st = m["source_checks"]["ivshape"]
        self.assertEqual(st["source"]["rows_on_or_after_seal_skipped"], sum(1 for d, _ in EXTRA if d >= rw.SEAL))
        self.assertGreaterEqual(st["source"]["rows_on_or_after_seal_skipped"], 1)
        self.assertEqual(st["source"]["rows_off_calendar"], 1)                   # the Saturday row
        self.assertEqual(st["source"]["duplicate_keys_quarantined"], 1)
        self.assertEqual(st["orientation_line"]["security_id"], SPY)

    def test_orientation_does_not_depend_on_the_vendor_sign_convention(self):
        """Every slope negated (the other convention): iv_skew_21 is bit-identical; iv_vov_21 never reads the slope."""
        a = Case(self.base, world(), "a")
        a.run("fa", FIELDS)
        b = Case(self.base, world(flip=-1.0), "b")
        mb = b.run("fb", FIELDS)
        for name in FIELDS:
            self.assertEqual(a.field("fa", name).tobytes(), b.field("fb", name).tobytes(), name)
        e = {x["name"]: x for x in mb["fields"]}
        self.assertEqual(e["iv_skew_21"]["sessions_oriented_negative"], 0)

    def test_point_in_time_probe(self):
        """Rows <= t never move when every vendor row dated session t or later moves; later rows do move."""
        for name in FIELDS:
            same, moved = self.probe(name, CUT)
            self.assertTrue(same, name)
            self.assertTrue(moved, name)

    def test_probe_fails_on_a_leaky_variant(self):
        """The probe has teeth: reading the decision session's own surface (LAG_SESSIONS 0) moves row CUT."""
        for name in FIELDS:
            same, _ = self.probe(name, CUT, [(ivs, "LAG_SESSIONS", 0)])
            self.assertFalse(same, name)

    def test_reuse_copies_identical_bytes(self):
        case = Case(self.base, world())
        case.run("a", FIELDS)
        sha = hashlib.sha256((self.base / "a" / "manifest.json").read_bytes()).hexdigest()
        m = case.run("b", FIELDS, reuse=self.base / "a", reuse_sha256=sha)
        for name in FIELDS:
            self.assertEqual((self.base / "a" / f"{name}.f64").read_bytes(), (self.base / "b" / f"{name}.f64").read_bytes())
        self.assertEqual(sorted(m["reuse"]["reused"]), sorted(FIELDS))

    def test_seal_and_refusals(self):
        rows = world()
        case = Case(self.base, rows)
        with self.assertRaisesRegex(ValueError, "need --price-source"):
            case.run("x1", FIELDS, module_options={})
        self.assertFalse((self.base / "x1").exists())
        with mock.patch.object(tool, "SEAL", dt.date(2026, 1, 1)):       # the builder's seal is not research_window's
            with self.assertRaisesRegex(rw.SealError, "not research_window's"):
                case.run("x2", ["iv_vov_21"])
        self.assertFalse((self.base / "x2" / "manifest.json").exists())
        noslope = Case(self.base, rows, "noslope", drop=("shD1",))
        with self.assertRaisesRegex(ValueError, "column shD1 is missing"):
            noslope.run("x3", FIELDS)
        self.assertFalse((self.base / "x3").exists())
        noslope.run("x3b", ["iv_vov_21"])                                 # vol-of-vol does not read the slope
        twin = {k: list(v) for k, v in rows.items()}                       # SPY on a second securityID
        for k, v in (("tradingDate", dt.date(2018, 7, 3)), ("securityID", 78), ("ticker_tk", "SPY"), ("close", 1.0),
                     ("cumulReturnFactor", 1.0), ("volume", 1.0), ("shD1", -0.1), ("atmCenI_21d", 0.2)):
            twin[k].append(v)
        two = Case(self.base, twin, "twin")
        with self.assertRaisesRegex(ValueError, "maps to 2 vendor securityIDs"):
            two.run("x4", ["iv_skew_21"])
        self.assertFalse((self.base / "x4" / "manifest.json").exists())
        argv = ["--role", str(case.role), "--role-sha256", case.role_sha, "--output", str(self.base / "cli"),
                "--fields", ",".join(FIELDS), "--price-source", str(case.src)]
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(argv)
        case.run("api", FIELDS)
        for name in FIELDS:
            self.assertEqual((self.base / "cli" / f"{name}.f64").read_bytes(),
                             (self.base / "api" / f"{name}.f64").read_bytes())

    def test_plain_builder_does_not_register_the_module(self):
        """Off by default: outside registered() the builder knows none of the fields."""
        self._reg.__exit__(None, None, None)
        try:
            for name in FIELDS:
                self.assertNotIn(name, tool.ALL_FIELDS)
            self.assertFalse(any(type(m).__module__ == "research_fields_ivshape" for m in tool.FIELD_MODULES))
        finally:
            self._reg = registered()
            self._reg.__enter__()


if __name__ == "__main__":
    unittest.main()
