"""research_fields_ohlc (platform v8 lane XWQ): synthetic vendor source and role only, no real archive access.

The world: a TickerHistory3-like parquet (dates mixed in every row group) from 2018-06 to 2019-03 for five role lines
and one line off the role axis, with a 2:1 split, cash dividends, per-line factor anchors that differ (the vendor's
cumulReturnFactor level is arbitrary across lines), an order-violating bar, a null open, a non-positive low, a duplicated
key, a Saturday row and rows after the seal. The role is the NYSE sessions 2018-07-02 .. 2019-02-28 projected from it
(close = raw close x factor). Every field is built through the builder's run() with the draft module registered,
checked cell by cell against the definition written out here independently, and put through a look-ahead probe that is
shown to fail on a leaky variant of the module."""
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
import prepare_research_fields_ohlc as oreg
import research_fields_ohlc as ohlc
import research_fields_sec as sec
import research_window as rw

EPOCH = dt.date(1970, 1, 1)
DAY_NS = 86_400_000_000_000
TH_FIRST, ROLE_FIRST, ROLE_LAST, TH_LAST = dt.date(2018, 6, 1), dt.date(2018, 7, 2), dt.date(2019, 2, 28), \
    dt.date(2019, 3, 29)
ROLE_IDS = [11, 22, 33, 44, 55]
OFF_ROLE = 99
FIELDS = list(oreg.FIELDS_OHLC_DRAFT)
ANCHOR = {11: 1.0, 22: 7.5, 33: 0.03, 44: 120.0, 55: 0.8, OFF_ROLE: 1.0}   # vendor factor levels differ by line
SPLIT = (22, dt.date(2018, 10, 15), 2.0)
DIVIDENDS = {33: [(dt.date(2018, 9, 14), 0.01), (dt.date(2018, 12, 14), 0.012)]}
BAD_ORDER = (44, dt.date(2018, 11, 5))          # high below the close: the bar is withheld whole
NULL_OPEN = (55, dt.date(2018, 8, 20))
ZERO_LOW = (11, dt.date(2019, 1, 9))
DUP_KEY = (33, dt.date(2018, 8, 1))
EXTRA = [(dt.date(2018, 9, 8), 11),            # a Saturday: off the calendar
         (dt.date(2025, 2, 3), 22), (dt.date(2024, 3, 1), 11)]   # on or after the seal
CUT = 90                                        # the probe's last unmutated role row


def day(d):
    return (d - EPOCH).days


def sessions(first, last):
    return [EPOCH + dt.timedelta(days=int(x)) for x in sec.nyse_sessions(first, last)]


def world(seed=20261002):
    """Vendor rows {column: list} (None = a null cell)."""
    rng = np.random.default_rng(seed)
    cal = sessions(TH_FIRST, TH_LAST)
    rows = {k: [] for k in ("tradingDate", "securityID", "open", "high", "low", "close", "volume",
                            "cumulReturnFactor")}
    for n, sid in enumerate(ROLE_IDS + [OFF_ROLE]):
        c, f = 20.0 + 13.0 * n, ANCHOR[sid]
        divs = dict(DIVIDENDS.get(sid, []))
        for d in cal:
            o = c * math.exp(rng.normal(0.0, 0.01))           # the open gaps from the previous raw close
            if (sid, d) == SPLIT[:2]:
                o /= SPLIT[2]
                f *= SPLIT[2]
            if d in divs:
                o *= 1.0 - divs[d]
                f /= 1.0 - divs[d]
            c = o * math.exp(rng.normal(0.0, 0.015))
            hi = max(o, c) * math.exp(abs(rng.normal(0.0, 0.006)))
            lo = min(o, c) * math.exp(-abs(rng.normal(0.0, 0.006)))
            if (sid, d) == BAD_ORDER:
                hi = c * 0.99
            if (sid, d) == ZERO_LOW:
                lo = 0.0
            row = {"tradingDate": d, "securityID": sid, "open": None if (sid, d) == NULL_OPEN else o, "high": hi,
                   "low": lo, "close": c, "volume": float(rng.integers(10_000, 90_000)), "cumulReturnFactor": f}
            for k, v in row.items():
                rows[k].append(v)
            if (sid, d) == DUP_KEY:
                for k, v in row.items():
                    rows[k].append(v if k != "close" else v * 1.01)
    for d, sid in EXTRA:
        for k, v in (("tradingDate", d), ("securityID", sid), ("open", 50.0), ("high", 51.0), ("low", 49.0),
                     ("close", 50.0), ("volume", 1e4), ("cumulReturnFactor", 1.0)):
            rows[k].append(v)
    return rows


TYPES = {"tradingDate": pa.date32(), "securityID": pa.int64(), "open": pa.float32(), "high": pa.float32(),
         "low": pa.float32(), "close": pa.float32(), "volume": pa.float64(), "cumulReturnFactor": pa.float64()}


def write_source(path, rows, drop=()):
    order = np.random.default_rng(7).permutation(len(rows["securityID"]))     # dates mixed in every row group
    pq.write_table(pa.table({k: pa.array([rows[k][i] for i in order], t) for k, t in TYPES.items() if k not in drop}),
                   path, row_group_size=300)
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def f32(x):
    return math.nan if x is None else float(np.float32(x))


def keyed(rows):
    """{(sid, epoch day): row index} of the unique keys, and the set of duplicated keys."""
    seen, keep = {}, {}
    for i, (d, s) in enumerate(zip(rows["tradingDate"], rows["securityID"])):
        seen[(s, day(d))] = seen.get((s, day(d)), 0) + 1
        keep[(s, day(d))] = i
    return {k: i for k, i in keep.items() if seen[k] == 1}, {k for k, c in seen.items() if c > 1}


def write_role(root, rows, source_sha):
    """The role projection's contract: close = f64(f32 raw close) x factor on unique valid keys."""
    root.mkdir(parents=True)
    keep, _ = keyed(rows)
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
            if r > 0 and f > 0 and v >= 0:
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
    """The draft module registered into the builder for the duration (undone on exit)."""
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        oreg.register(vars(tool))
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

    def role_matrix(self, name):
        return np.fromfile(self.role / name, dtype="<f8").reshape(len(self.days), len(ROLE_IDS))


def reference(rows, days):
    """The field definition written out independently: {name: (dates x lines)}."""
    keep, _ = keyed(rows)
    out = {x: np.full((len(days), len(ROLE_IDS)), np.nan) for x in FIELDS}
    for t, d in enumerate(days):
        for j, sid in enumerate(ROLE_IDS):
            i = keep.get((sid, d))
            if i is None:
                continue
            r, f = f32(rows["close"][i]), rows["cumulReturnFactor"][i]
            o, hi, lo = f32(rows["open"][i]), f32(rows["high"][i]), f32(rows["low"][i])
            if not (r > 0 and f > 0) or any(not (math.isfinite(v) and v > 0) for v in (o, hi, lo)):
                continue
            if lo > min(o, r) or hi < max(o, r):
                continue
            k = (r * f) / r
            out["open_adj"][t, j], out["high_adj"][t, j], out["low_adj"][t, j] = o * k, hi * k, lo * k
    return out


def mutated(rows, first_day, seed=5):
    """Every vendor row dated on or after ``first_day`` (every line) moved at random."""
    rng = np.random.default_rng(seed)
    late = {k: list(v) for k, v in rows.items()}
    for i, d in enumerate(late["tradingDate"]):
        if day(d) >= first_day:
            m = float(rng.uniform(0.5, 1.5))
            for k in ("open", "high", "low", "close"):
                if late[k][i] is not None:
                    late[k][i] *= m * float(rng.uniform(0.98, 1.02)) if k == "open" else m
            late["cumulReturnFactor"][i] *= float(rng.uniform(0.9, 1.1))
    return late


class OhlcFields(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self._reg = registered()
        self._reg.__enter__()

    def tearDown(self):
        self._reg.__exit__(None, None, None)
        self._tmp.cleanup()

    def probe(self, name, cut_row, patches=()):
        """The look-ahead probe: build ``name`` on the world and on the world with every row dated after session
        ``cut_row`` moved; returns (rows <= cut_row bit-identical, rows after cut_row differ)."""
        rows = world()
        days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]
        with contextlib.ExitStack() as stack:
            for target, attr, value in patches:
                stack.enter_context(mock.patch.object(target, attr, value))
            tag = f"{name}-{len(list(self.base.iterdir()))}"
            a = Case(self.base, rows, tag + "a")
            a.run(tag + "fa", [name])
            b = Case(self.base, mutated(rows, days[cut_row + 1]), tag + "b")
            b.run(tag + "fb", [name])
        x, y = a.field(tag + "fa", name), b.field(tag + "fb", name)
        return x[:cut_row + 1].tobytes() == y[:cut_row + 1].tobytes(), x[cut_row + 1:].tobytes() != y[cut_row + 1:].tobytes()

    def test_values_match_definition(self):
        rows = world()
        case = Case(self.base, rows)
        m = case.run("f", FIELDS)
        ref = reference(rows, case.days)
        col = {sid: j for j, sid in enumerate(ROLE_IDS)}
        for name in FIELDS:
            got = case.field("f", name)
            self.assertTrue(np.array_equal(np.isnan(got), np.isnan(ref[name])), name)
            ok = np.isfinite(ref[name])
            self.assertTrue(np.array_equal(got[ok], ref[name][ok]), name)            # bit for bit
            self.assertGreater(int(ok.sum()), 0.95 * got.size, name)
        t_of = {d: t for t, d in enumerate(case.days)}
        for (sid, d), withheld in ((BAD_ORDER, FIELDS), (NULL_OPEN, FIELDS), (ZERO_LOW, FIELDS),
                                   (DUP_KEY, FIELDS)):
            for x in withheld:
                self.assertTrue(math.isnan(case.field("f", x)[t_of[day(d)], col[sid]]), (sid, d, x))
        e = {x["name"]: x for x in m["fields"]}
        for name in FIELDS:
            self.assertTrue(e[name]["point_in_time"])
            self.assertEqual(e[name]["producer"]["module"], "research_fields_ohlc.py")
            self.assertEqual(e[name]["lag_sessions"], 0)
            self.assertEqual(e[name]["formula_id"], ohlc.FIELDS[name]["formula_id"])
            self.assertEqual(e[name]["formula_sha256"], tool.formula_id(name, tool.spec_definition(name, 0)))
            self.assertEqual(e[name]["order_violation_member_cells"], 1)              # BAD_ORDER (a member cell)
            self.assertGreaterEqual(e[name]["missing_bar_member_cells"], 2)           # NULL_OPEN, ZERO_LOW
        st = m["source_checks"]["ohlc"]["source"]
        self.assertEqual(st["rows_on_or_after_seal_skipped"], sum(1 for d, _ in EXTRA if d >= rw.SEAL))
        self.assertGreaterEqual(st["rows_on_or_after_seal_skipped"], 1)
        self.assertEqual(st["rows_off_calendar"], 1)                                  # the Saturday row
        self.assertEqual(st["duplicate_keys_quarantined"], 1)

    def test_basis_is_the_role_close(self):
        """x_adj / close equals the vendor bar's x / raw close (same session), and across the split the adjusted open
        continues the previous adjusted close while the raw open halves."""
        rows = world()
        case = Case(self.base, rows)
        case.run("f", FIELDS)
        close, raw = case.role_matrix("close.f64"), case.role_matrix("raw_close.f64")
        keep, _ = keyed(rows)
        for name, col in (("open_adj", "open"), ("high_adj", "high"), ("low_adj", "low")):
            got = case.field("f", name)
            for t, d in enumerate(case.days):
                for j, sid in enumerate(ROLE_IDS):
                    if math.isfinite(got[t, j]):
                        want = f32(rows[col][keep[(sid, d)]]) / raw[t, j]
                        self.assertTrue(math.isclose(got[t, j] / close[t, j], want, rel_tol=1e-14), (name, t, sid))
        t = case.days.index(day(SPLIT[1]))
        j = ROLE_IDS.index(SPLIT[0])
        o = case.field("f", "open_adj")
        self.assertLess(abs(o[t, j] / close[t - 1, j] - 1.0), 0.1)
        raw_open = f32(rows["open"][keep[(SPLIT[0], day(SPLIT[1]))]])
        self.assertLess(abs(raw_open / raw[t - 1, j] - 0.5), 0.05)

    def test_point_in_time_probe(self):
        """Rows <= t never move when every vendor row dated after session t moves; later rows do move."""
        for name in FIELDS:
            same, moved = self.probe(name, CUT)
            self.assertTrue(same, name)
            self.assertTrue(moved, name)

    def test_probe_fails_on_a_leaky_variant(self):
        """The probe has teeth: the module reading the next session's bar (LAG_SESSIONS -1) moves row CUT."""
        for name in FIELDS:
            same, _ = self.probe(name, CUT, [(ohlc, "LAG_SESSIONS", -1)])
            self.assertFalse(same, name)

    def test_reuse_copies_identical_bytes(self):
        rows = world()
        case = Case(self.base, rows)
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
                case.run("x2", ["open_adj"])
        self.assertFalse((self.base / "x2" / "manifest.json").exists())
        nohigh = Case(self.base, rows, "nohigh", drop=("high",))
        with self.assertRaisesRegex(ValueError, "column high is missing"):
            nohigh.run("x3", ["open_adj"])
        self.assertFalse((self.base / "x3").exists())
        with self.assertRaisesRegex(ValueError, "differs from the role's source_sha256"):
            case.run("x4", FIELDS, module_options={"price_source": write_other(self.base, rows)})
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
            self.assertFalse(any(type(m).__module__ == "research_fields_ohlc" for m in tool.FIELD_MODULES))
        finally:
            self._reg = registered()
            self._reg.__enter__()


def write_other(base, rows):
    """A different vendor file (one row more): its SHA-256 is not the role's."""
    late = {k: list(v) for k, v in rows.items()}
    for k, v in (("tradingDate", dt.date(2018, 6, 4)), ("securityID", 777), ("open", 1.0), ("high", 1.0),
                 ("low", 1.0), ("close", 1.0), ("volume", 1.0), ("cumulReturnFactor", 1.0)):
        late[k].append(v)
    path = base / "other-source.parquet"
    write_source(path, late)
    return path


if __name__ == "__main__":
    unittest.main()
