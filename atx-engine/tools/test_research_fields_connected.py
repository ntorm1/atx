"""research_fields_connected (platform v8 lane YDATA round 2): a synthetic 13F stage, role and builder fixtures only.

Pure tests check FCAP (Anton-Polk's common-ownership formula) and its filer-chunk invariance, the within-book active
share, the top-K and average-rank weights with ties, the connected return and the 63-session window by hand. The field
test builds an eight-line role (two lines without shares_out, one line dropped from membership at the Q3 quarter end)
and a three-quarter 13F world (two index-like books holding a fixed fraction of shares_out, five concentrated books, a
single-node book, a book of non-nodes), checks every cell of ``conn_ret63`` against the definition written out here
with plain loops (TOP_K 3 and CONN_MIN 2 in this small world), then probes look-ahead on the 13F clock (shown to fail
on a same-session-mark variant) and on the role closes (shown to fail on a lag-0 variant)."""
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

import prepare_research_fields as tool
import prepare_research_fields_ydata as yreg
import research_fields_connected as con
import research_fields_holdings as hold
import research_window as rw
import test_prepare_research_fields as base
import test_research_fields_holdings as thold
import test_research_fields_mgr13f as tmgr

IDS = [101, 202, 303, 404, 505, 606, 707, 808]
SESSIONS = thold.SESSIONS            # 2024-05-01 .. 2024-12-31 weekdays
QS = [dt.date(2024, 3, 31), dt.date(2024, 6, 30), dt.date(2024, 9, 30)]
SECS = {f"C{s}": s for s in IDS + [909]}                      # 909 is mapped but not a role line; CUNM is unmapped
PRICE = {"C101": 50.0, "C202": 40.0, "C303": 30.0, "C404": 20.0, "C505": 25.0, "C606": 60.0, "C707": 70.0,
         "C808": 80.0, "C909": 90.0, "CUNM": 10.0}
T_Q3 = SESSIONS.index(dt.date(2024, 9, 30))
NOT_MEMBER = (T_Q3, 707)             # 707 is not a member at the Q3 quarter-end session: not a Q3 node
INDEX_LIKE = {"1": 0.05, "9": 0.02}  # books holding a fixed fraction of every line's shares_out
BOOKS = {
    "2": {"C101": 400, "C202": 300, "C606": 50},
    "3": {"C202": 500, "C303": 800, "C707": 100},
    "4": {"C606": 900, "C707": 100, "C808": 300},
    "5": {"C101": 100, "C808": 700, "C303": 50, "C404": 500},
    "6": {"C101": 50, "C202": 50, "C303": 50, "C606": 50, "C707": 50, "C808": 900},
    "7": {"C606": 1000},                                                  # one node: no pair
    "8": {"C404": 100, "C909": 100, "C101": 10, "CUNM": 100},              # one node among non-nodes
}
ACTIVE = {2, 3, 4, 5, 6}             # 505 is a node held only by the index-like books: no connection
Q3_BOOKS = {"2": {"C101": 400, "C202": 300, "C808": 50}, "4": {"C606": 900, "C808": 300}}
K, CMIN = 3, 2
RECIPE = ["si_shares", "shares_out", con.NAME]
CUT = dt.date(2024, 11, 18)          # Q3 becomes visible on 2024-11-19 (V = 11-15 22:00 UTC)
PRICE_CUT = SESSIONS.index(dt.date(2024, 10, 15))


@contextlib.contextmanager
def registered():
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)), \
            mock.patch.object(con, "TOP_K", K), mock.patch.object(con, "CONN_MIN", CMIN):
        yreg.register(vars(tool))
        yield


@contextlib.contextmanager
def world_globals():
    with mock.patch.object(tmgr, "QS", QS), mock.patch.object(tmgr, "SECS", SECS):
        yield


def quarter_session(q):
    t = max((i for i, d in enumerate(SESSIONS) if d <= q), default=-1)
    return t if t >= 0 else None


def holdings_world(so):
    """{(filer, quarter): [(cusip, shares, value)]}: the index-like books hold a fixed fraction of each line's
    shares_out at the quarter-end session (cap weights exactly), the other books fixed share counts."""
    out = {}
    for qi, q in enumerate(QS):
        px = {c: p * (1.0 + 0.1 * qi) for c, p in PRICE.items()}
        tq = quarter_session(q)
        for k, frac in INDEX_LIKE.items():
            rows = []
            for c, sid in SECS.items():
                o = so[tq, IDS.index(sid)] if tq is not None and sid in IDS else math.nan
                sh = frac * o if math.isfinite(o) and o > 0 else 100.0
                rows.append((c, sh, sh * px[c]))
            out[(k, q)] = rows
        books = dict(BOOKS, **(Q3_BOOKS if q == QS[-1] else {}))
        for k, book in books.items():
            out[(k, q)] = [(c, float(s), float(s) * px[c]) for c, s in book.items()]
    return out


def write_role(root: Path, source_sha: str, close_from=None) -> str:
    root.mkdir(parents=True)
    nd = len(SESSIONS)
    with mock.patch.object(base, "IDS", IDS):
        close, raw, present = base.role_prices(nd)
    if close_from is not None:   # the price probe: every close after session close_from moves
        close[close_from + 1:] *= np.exp(0.03 * np.arange(1, nd - close_from))[:, None]
    member = np.ones((nd, len(IDS)), dtype="u1")
    member[0:3, 4] = 0
    member[NOT_MEMBER[0], IDS.index(NOT_MEMBER[1])] = 0
    blobs = {"sessions.i64": np.array([base.day(d) * base.DAY_NS for d in SESSIONS], dtype="<i8").tobytes(),
             "ids.u64": np.array(IDS, dtype="<u8").tobytes(), "member.u8": member.tobytes(),
             "close.f64": close.astype("<f8").tobytes(), "raw_close.f64": raw.astype("<f8").tobytes(),
             "present.u8": present.tobytes(),
             "volume.f64": np.where(present != 0, 1e4, np.nan).astype("<f8").tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": nd, "instruments": len(IDS),
                "instrument_namespace": "spiderrock.securityID", "score_begin": 10, "score_end": nd,
                "source_sha256": source_sha, "files": files, "clock_recipe": "modeled-session+22h-mark+23h-decision-v1"}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return base.sha(root / "manifest.json")


class Fixture(base.Fixture):
    def __init__(self, root: Path, mutate_after=None, close_from=None):
        self.base = root
        self.th = root / "TickerHistory3.parquet"
        with mock.patch.object(base, "IDS", IDS):
            base.write_tickerhistory(self.th)
        self.finra = root / "finra"
        base.write_finra(self.finra)
        self.lake = root / "lake"
        base.write_lake(self.lake)
        self.role = root / "role"
        self.role_sha = write_role(self.role, thold.sha(self.th), close_from)
        self.run("so", fields=["si_shares", "shares_out"], module_options={})
        self.so = self.field("so", "shares_out")
        self.world = holdings_world(self.so)
        self.stage = root / "thirteenf"
        with world_globals():
            self.stage_sha = tmgr.write_stage(self.stage, self.world, mutate_after)
        self.close = np.frombuffer((self.role / "close.f64").read_bytes(), dtype="<f8").reshape(len(SESSIONS), len(IDS))
        self.present = np.frombuffer((self.role / "present.u8").read_bytes(), dtype="u1").reshape(len(SESSIONS),
                                                                                                  len(IDS)) != 0
        self.member = np.frombuffer((self.role / "member.u8").read_bytes(), dtype="u1").reshape(len(SESSIONS),
                                                                                                len(IDS)) != 0

    def run(self, out, **kw):
        if "module_options" not in kw:
            kw["module_options"] = {"mgr13f_stage": self.stage, "mgr13f_stage_sha256": self.stage_sha}
        return super().run(out, **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(SESSIONS), len(IDS))


# ---------------------------------------------------------------------------------------------------------------------
# The definition written out with plain loops (the oracle)
# ---------------------------------------------------------------------------------------------------------------------

def oracle_networks(fx):
    """{quarter: {sid: [(neighbour sid, weight)]}} and {quarter: active filers}."""
    with world_globals():
        pos = tmgr.positions(fx.world)
    nets, actives = {}, {}
    for q in QS:
        tq = quarter_session(q)
        if tq is None:
            continue
        p, px = pos[q]
        nodes = [sid for j, sid in enumerate(IDS) if fx.member[tq, j] and math.isfinite(fx.so[tq, j])
                 and fx.so[tq, j] > 0 and sid in px]
        cap = {sid: fx.so[tq, IDS.index(sid)] * px[sid] for sid in nodes}
        books = {}
        for (f, sid), (sh, _) in p.items():
            if sid in cap:
                books.setdefault(f, {})[sid] = sh * px[sid]
        active = set()
        for f, b in books.items():
            if len(b) < 2:
                continue
            total, ctotal = sum(b.values()), sum(cap[s] for s in b)
            if 0.5 * sum(abs(b[s] / total - cap[s] / ctotal) for s in b) >= 0.2:
                active.add(f)
        fcap = {}
        for i in nodes:
            for j in nodes:
                if i != j:
                    num = sum(books[f][i] + books[f][j] for f in active if i in books[f] and j in books[f])
                    fcap[(i, j)] = num / (cap[i] + cap[j])
        pairs = [v for (i, j), v in fcap.items() if i < j and v > 0]
        m = len(pairs)

        def weight(v):
            lo, eq = sum(x < v for x in pairs), sum(x == v for x in pairs)
            return (lo + (eq + 1) / 2.0) / m

        kept = {}
        for i in nodes:
            cands = sorted((j for j in nodes if j != i and fcap[(i, j)] > 0), key=lambda j: (-fcap[(i, j)], IDS.index(j)))
            if cands:
                kept[i] = [(j, weight(fcap[(i, j)])) for j in cands[:K]]
        nets[q], actives[q] = kept, active
    return nets, actives


def oracle(fx):
    nets, actives = oracle_networks(fx)
    vis = {q: dt.datetime.combine(tmgr.deadline(q), dt.time(0), dt.timezone.utc) + dt.timedelta(hours=46) for q in QS}
    out = np.full((len(SESSIONS), len(IDS)), np.nan)
    for t, d in enumerate(SESSIONS):
        if t == 0:
            continue
        mark = dt.datetime.combine(SESSIONS[t - 1], dt.time(22), dt.timezone.utc)
        ret = {}
        e = t - 1                                    # the 63-session return ends at the previous session
        for j, sid in enumerate(IDS):
            ret[sid] = math.nan
            if e >= 63:
                c1, c0 = fx.close[e, j], fx.close[e - 63, j]
                if fx.present[e, j] and fx.present[e - 63, j] and math.isfinite(c1) and math.isfinite(c0) \
                        and c1 > 0 and c0 > 0:
                    ret[sid] = c1 / c0 - 1.0
        for j, sid in enumerate(IDS):
            cands = [q for q in nets if vis[q] < mark and (d - q).days <= 150 and q <= d and sid in nets[q]]
            if not cands:
                continue
            valid = [(w, ret[n]) for n, w in nets[max(cands)][sid] if math.isfinite(ret[n])]
            if len(valid) >= CMIN:
                out[t, j] = sum(w * r for w, r in valid) / sum(w for w, _ in valid)
    return out, nets, actives


# ---------------------------------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------------------------------

class Pure(unittest.TestCase):
    def test_fcap_by_hand_and_chunk_invariance(self):
        # caps 100, 200, 300; filer 0 holds nodes 0, 1 (10, 20); filer 1 holds 1, 2 (30, 60); filer 2 holds all (5 each)
        fi = np.array([0, 0, 1, 1, 2, 2, 2])
        ni = np.array([0, 1, 1, 2, 0, 1, 2])
        va = np.array([10.0, 20.0, 30.0, 60.0, 5.0, 5.0, 5.0])
        cap = np.array([100.0, 200.0, 300.0])
        f = con.fcap_matrix(fi, ni, va, cap, 3)
        want = np.array([[0.0, 40.0 / 300.0, 10.0 / 400.0], [40.0 / 300.0, 0.0, 100.0 / 500.0],
                         [10.0 / 400.0, 100.0 / 500.0, 0.0]])
        np.testing.assert_allclose(f, want, rtol=1e-15, atol=0)
        self.assertEqual(f.tobytes(), f.T.tobytes())                     # exactly symmetric
        np.testing.assert_allclose(con.fcap_matrix(fi, ni, va, cap, 3, chunk=1), f, rtol=1e-15, atol=0)

    def test_active_share_by_hand(self):
        # filer 0: cap weights (AS 0); filer 1: one node; filer 2: AS 0.5; filer 3: AS 0.15
        fi = np.array([0, 0, 1, 2, 2, 3, 3])
        ni = np.array([0, 1, 0, 0, 1, 0, 1])
        cap = np.array([1.0, 3.0])
        va = np.array([2.0, 6.0, 5.0, 0.75 * 4.0, 0.25 * 4.0, 0.4 * 4.0, 0.6 * 4.0])
        active, share, count = con.active_owners(fi, ni, va, cap, 4)
        self.assertEqual(active.tolist(), [False, False, True, False])
        np.testing.assert_allclose(share, [0.0, 0.0, 0.5, 0.15], atol=1e-15)
        self.assertEqual(count.tolist(), [2, 1, 2, 2])
        self.assertEqual(con.ACTIVE_SHARE_MIN, 0.2)

    def test_top_connections_ties_and_ranks(self):
        f = np.zeros((4, 4))
        for (i, j), v in {(0, 1): 0.3, (0, 2): 0.3, (0, 3): 0.1, (1, 2): 0.2, (2, 3): 0.1}.items():
            f[i, j] = f[j, i] = v
        nbr, w, m = con.top_connections(f, k=2)       # pairs 0.1, 0.1, 0.2, 0.3, 0.3: average ranks 1.5, 3, 4.5
        self.assertEqual(m, 5)
        self.assertEqual(nbr.tolist(), [[1, 2], [0, 2], [0, 1], [0, 2]])   # ties: the lower column first
        np.testing.assert_allclose(w, [[0.9, 0.9], [0.9, 0.6], [0.9, 0.6], [0.3, 0.3]], rtol=1e-15)
        nbr3, w3, _ = con.top_connections(f, k=3)
        self.assertEqual(nbr3[1].tolist(), [0, 2, -1])                     # FCAP 0 is no connection
        self.assertEqual(w3[1, 2], 0.0)
        nbr9, _, _ = con.top_connections(f, k=9)                           # k above the node count: padded
        self.assertEqual(nbr9.shape, (4, 9))
        self.assertEqual(con.TOP_K, 50)

    def test_connected_value_and_window(self):
        nbr = np.array([[0, 1, 2, -1], [3, -1, -1, -1]])
        w = np.array([[0.5, 0.25, 0.25, 0.0], [1.0, 0.0, 0.0, 0.0]])
        ret = np.array([0.1, np.nan, 0.3, 0.2])
        with mock.patch.object(con, "CONN_MIN", 2):
            v, cnt = con.connected_value(nbr, w, ret)
        self.assertAlmostEqual(v[0], (0.05 + 0.075) / 0.75, places=15)
        self.assertTrue(math.isnan(v[1]))                                  # one valid return < CONN_MIN 2
        self.assertEqual(cnt.tolist(), [2, 1])
        self.assertEqual(con.CONN_MIN, 10)
        size = con.WINDOW + con.LAG_SESSIONS + 1
        ring_c = np.full((size, 3), np.nan)
        ring_p = np.zeros((size, 3), dtype=bool)
        for t in range(71):
            ring_c[t % size] = [1.0 + t, 2.0, 0.0]
            ring_p[t % size] = [True, t != 5, True]
            r = con.window_returns(ring_c, ring_p, t)
            if t < 64:
                self.assertTrue(np.isnan(r).all())                         # before row WINDOW + LAG
            elif t == 69:
                self.assertTrue(math.isnan(r[1]))                          # row 68 vs row 5, which is absent
                self.assertAlmostEqual(r[0], 69.0 / 6.0 - 1.0, places=15)
        self.assertAlmostEqual(r[0], 70.0 / 7.0 - 1.0, places=15)          # at t = 70: row 69 vs row 6
        self.assertEqual(r[1], 0.0)
        self.assertTrue(math.isnan(r[2]))                                  # non-positive close
        with self.assertRaisesRegex(ValueError, "shorter than the window"):
            con.window_returns(np.ones((size - 1, 3)), np.ones((size - 1, 3), dtype=bool), 70)


class ConnectedField(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._reg = registered()
        cls._reg.__enter__()
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = Fixture(Path(cls.temp.name))
        cls.manifest = cls.fx.run("fields", fields=RECIPE)
        cls.got = cls.fx.field("fields", con.NAME)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
        cls._reg.__exit__(None, None, None)

    def test_values_match_the_definition(self):
        want, nets, actives = oracle(self.fx)
        self.assertTrue(np.array_equal(np.isnan(self.got), np.isnan(want)))
        ok = np.isfinite(want)
        self.assertGreater(int(ok.sum()), 300)
        np.testing.assert_allclose(self.got[ok], want[ok], rtol=1e-12, atol=1e-15)
        # the world: Q1 has no role session (no network); the index-like and single-node books are not owners
        self.assertEqual(sorted(nets), QS[1:])
        for q in QS[1:]:
            self.assertEqual(actives[q], ACTIVE, q)
        self.assertNotIn(707, nets[QS[2]])                                    # not a member at the Q3 quarter end
        self.assertIn(707, nets[QS[1]])
        for sid in (404, 505):                    # 404: no shares_out (not a node); 505: a node with no active owner
            self.assertNotIn(sid, nets[QS[1]])
            self.assertTrue(np.isnan(self.got[:, IDS.index(sid)]).all())
        j707 = IDS.index(707)
        t_in, t_out = SESSIONS.index(dt.date(2024, 11, 27)), SESSIONS.index(dt.date(2024, 11, 29))
        self.assertTrue(math.isfinite(self.got[t_in, j707]))                  # Q2's network, still fresh
        self.assertTrue(math.isnan(self.got[t_out, j707]))                    # Q2 stale after 150 days
        per_q = self.manifest["source_checks"]["connected"]["thirteenf"]["per_quarter"]
        self.assertEqual(per_q["2024-03-31"]["not_built"], "the quarter end is before the first role session")
        for q, nodes in (("2024-06-30", 7), ("2024-09-30", 6)):
            self.assertEqual(per_q[q]["nodes"], nodes)
            self.assertEqual(per_q[q]["nodes_with_connections"], nodes - 1)       # 505
            self.assertEqual(per_q[q]["active_owners"], len(ACTIVE))
            self.assertEqual(per_q[q]["filers_index_like"], 2)                 # filers 1 and 9
            self.assertEqual(per_q[q]["filers_single_node"], 2)                # filers 7 and 8

    def test_clock_and_entry(self):
        t, j = SESSIONS.index(CUT), IDS.index(101)
        e = {x["name"]: x for x in self.manifest["fields"]}[con.NAME]
        self.assertEqual(e["producer"]["module"], "research_fields_connected.py")
        self.assertEqual(e["depends_on"], ["shares_out"])
        self.assertEqual(e["stage_manifest_sha256"], self.fx.stage_sha)
        self.assertEqual(e["imported_code"], con.imported_code("y_conn"))
        self.assertEqual(e["constants"], {"active_share_min": 0.2, "top_k": K, "window": 63, "lag_sessions": 1,
                                          "conn_min": CMIN})
        self.assertEqual(e["formula_id"], "ap-connected-ret63-13f-v1")
        clock = self.manifest["source_checks"]["connected"]["thirteenf"]["quarter_visible_at"]
        self.assertEqual(clock["2024-09-30"], "2024-11-15T22:00:00.000000000Z")
        self.assertTrue(math.isfinite(self.got[t, j]))

    def test_point_in_time_probe_13f(self):
        """Every holding available at or after the t-1 mark of CUT rescaled: rows <= CUT identical, later rows move; a
        variant reading the decision session's own mark moves row CUT (the probe has teeth)."""
        mark = dt.datetime.combine(SESSIONS[SESSIONS.index(CUT) - 1], dt.time(22), dt.timezone.utc)
        t = SESSIONS.index(CUT)
        with tempfile.TemporaryDirectory() as temp:
            moved = Fixture(Path(temp), mutate_after=mark)
            moved.run("f", fields=RECIPE)
            y = moved.field("f", con.NAME)
            self.assertEqual(self.got[:t + 1].tobytes(), y[:t + 1].tobytes())
            self.assertNotEqual(self.got[t + 1:].tobytes(), y[t + 1:].tobytes())
            same_mark = lambda days: days.astype(np.int64) * hold.DAY_NS + hold.MARK_NS
            with mock.patch.object(hold, "prev_marks", same_mark):
                self.fx.run("leak-a", fields=RECIPE)
                moved.run("leak-b", fields=RECIPE)
            a, b = self.fx.field("leak-a", con.NAME), moved.field("leak-b", con.NAME)
            self.assertNotEqual(a[:t + 1].tobytes(), b[:t + 1].tobytes())

    def test_point_in_time_probe_prices(self):
        """Every role close after session PRICE_CUT moves: rows <= PRICE_CUT + 1 identical (the return ends at t-1),
        later rows move; with LAG_SESSIONS 0 row PRICE_CUT + 1 moves (the probe has teeth)."""
        edge = PRICE_CUT + 1
        with tempfile.TemporaryDirectory() as temp:
            moved = Fixture(Path(temp), close_from=PRICE_CUT)
            self.assertEqual(moved.stage_sha, self.fx.stage_sha)
            moved.run("f", fields=RECIPE)
            y = moved.field("f", con.NAME)
            self.assertEqual(self.got[:edge + 1].tobytes(), y[:edge + 1].tobytes())
            self.assertNotEqual(self.got[edge + 1:].tobytes(), y[edge + 1:].tobytes())
            with mock.patch.object(con, "LAG_SESSIONS", 0):
                self.fx.run("lag0-a", fields=RECIPE)
                moved.run("lag0-b", fields=RECIPE)
            a, b = self.fx.field("lag0-a", con.NAME), moved.field("lag0-b", con.NAME)
            self.assertEqual(a[:edge].tobytes(), b[:edge].tobytes())
            self.assertNotEqual(a[edge].tobytes(), b[edge].tobytes())

    def test_refusals_and_seal(self):
        with self.assertRaisesRegex(ValueError, "need --mgr13f-stage"):
            self.fx.run("x1", fields=RECIPE, module_options={})
        self.assertFalse((self.fx.base / "x1").exists())
        with self.assertRaisesRegex(ValueError, "requires shares_out"):
            self.fx.run("x2", fields=[con.NAME])
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.fx.run("x3", fields=RECIPE, module_options={"mgr13f_stage": self.fx.stage,
                                                             "mgr13f_stage_sha256": "0" * 64})
        self.assertFalse((self.fx.base / "x3" / "manifest.json").exists())
        with mock.patch.object(tool, "SEAL", dt.date(2026, 1, 1)):
            with self.assertRaisesRegex(rw.SealError, "not research_window's"):
                self.fx.run("x4", fields=RECIPE)
        self.assertFalse((self.fx.base / "x4" / "manifest.json").exists())

    def test_reuse_and_cli(self):
        sha = hashlib.sha256((self.fx.base / "fields" / "manifest.json").read_bytes()).hexdigest()
        m = self.fx.run("again", fields=RECIPE, reuse=self.fx.base / "fields", reuse_sha256=sha)
        self.assertIn(con.NAME, m["reuse"]["reused"])
        self.assertEqual((self.fx.base / "again" / f"{con.NAME}.f64").read_bytes(),
                         (self.fx.base / "fields" / f"{con.NAME}.f64").read_bytes())
        argv = ["--role", str(self.fx.role), "--role-sha256", self.fx.role_sha, "--output", str(self.fx.base / "cli"),
                "--fields", ",".join(RECIPE), "--finra", str(self.fx.finra), "--tickerhistory", str(self.fx.th),
                "--lake", str(self.fx.lake), "--mgr13f-stage", str(self.fx.stage),
                "--mgr13f-stage-sha256", self.fx.stage_sha]
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(argv)                     # both 13F modules registered: the stage options are declared once
        self.assertEqual((self.fx.base / "cli" / f"{con.NAME}.f64").read_bytes(),
                         (self.fx.base / "fields" / f"{con.NAME}.f64").read_bytes())

    def test_plain_builder_does_not_register_the_module(self):
        self.assertTrue(any(type(m).__module__ == "research_fields_connected" for m in tool.FIELD_MODULES))
        self._reg.__exit__(None, None, None)
        try:
            self.assertNotIn(con.NAME, tool.ALL_FIELDS)
            self.assertFalse(any(type(m).__module__ == "research_fields_connected" for m in tool.FIELD_MODULES))
        finally:
            type(self)._reg = registered()
            self._reg.__enter__()


if __name__ == "__main__":
    unittest.main()
