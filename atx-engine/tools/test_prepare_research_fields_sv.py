"""sv_ratio126 (FINRA daily short sale volume; library v6.1): synthetic checks, no real archive access.

The reference below re-derives the field with the short-interest producer's OWN symbol map
(build-equity/audits/iteration21_finra_si_asof.py: canon, build_maps, and its collision loop transcribed) and
a brute-force window, so the builder's port of the map and the ratio/lag/min-count/zero-total rules are checked
against an independent implementation. The byte-identity test runs the full 40-field recipe (legacy + issuer)
with and without sv_ratio126 and compares every other field's bytes and manifest entry.
"""
import datetime as dt
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_research_fields as tool
import test_prepare_research_fields as base

REPO = Path(__file__).resolve().parents[2]
PRODUCER = REPO / "build-equity" / "audits" / "iteration21_finra_si_asof.py"
SESSIONS, IDS = base.SESSIONS, base.IDS
FIRST_FILE = dt.date(2024, 3, 1)
LAST_FILE = dt.date(2024, 12, 31)           # == the last role session: listed, never read
MISSING = dt.date(2024, 10, 15)             # a role session without a CNMS file
WEEKEND = dt.date(2024, 10, 12)             # a file dated off the role calendar: never read
AFTER = dt.date(2025, 1, 2)                 # after the role (and the seal): never read
DUP_DAY = dt.date(2024, 11, 6)              # AAA twice in one file: the last row wins
X_NEW = dt.date(2024, 9, 16)                # 404's vendor ticker OLD -> NEW
AMB = (dt.date(2024, 8, 5), dt.date(2024, 8, 9))  # 999 (not a role id) spells A.AA: AAA ambiguous
Z_CPK = dt.date(2024, 10, 16)               # CPK TotalVolume is 0 before this date
LATE_START = dt.date(2024, 9, 2)            # LATE (505) is in the files from this date on
BLANK = (dt.date(2024, 6, 3), 202)          # a blank vendor ticker: 202 unmapped that day
NULL = (dt.date(2024, 6, 4), 505)           # a null vendor ticker


def load_producer():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("_iteration21_finra_si_asof_for_sv_test", PRODUCER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ticker_of(sid, d):
    if (d, sid) == BLANK:
        return "  "
    if (d, sid) == NULL:
        return None
    return {101: "AAA", 202: "BF.B", 303: "CPK", 404: "OLD" if d < X_NEW else "NEW", 505: "LATE",
            999: "A.AA" if AMB[0] <= d <= AMB[1] else "ZZZ"}[sid]


def add_tickers(path):
    """The base fixture's TickerHistory plus a ticker_tk column (row order and groups kept)."""
    table = pq.read_table(path)
    days = table.column("tradingDate").to_pylist()
    sids = table.column("securityID").to_pylist()
    table = table.append_column("ticker_tk", pa.array([ticker_of(s, d) for s, d in zip(sids, days)], pa.string()))
    pq.write_table(table, path, row_group_size=700)


def file_days():
    days = [FIRST_FILE + dt.timedelta(days=i) for i in range((LAST_FILE - FIRST_FILE).days + 1)]
    return [d for d in days if d.weekday() < 5 and d != MISSING] + [WEEKEND, AFTER]


def read_days():
    """The files the builder must read: dated on the calendar (126 prefix file dates + role sessions) before the
    last role session."""
    dated = sorted(d for d in file_days() if d < SESSIONS[-1] and d != WEEKEND)
    return [d for d in dated if d >= [x for x in dated if x < SESSIONS[0]][-126]]


def cnms_rows(d, k):
    """(symbol, ShortVolume text, TotalVolume text) of the file dated d (k = its position)."""
    rows = [("AAA", "1", "2")] if d == DUP_DAY else []
    rows.append(("AAA", str(100 + k % 17), str(1000 + (k % 5) * 10)))
    if d.weekday() == 2:
        rows.append(("AA/A", "999", "1000"))           # canonical AAA, not the exact spelling: dropped
    rows.append(("BF/B", str(30 + k % 7), "100"))       # class share: canonical-only match to BF.B
    rows += [("CPK", str(20 + k % 3), "100") if d >= Z_CPK else ("CPK", "0", "0"),
             ("CpK", "99", "100"),                       # Citigroup preferred K -> CPRK, never CPK
             ("OLD", "1", "10"), ("NEW", "9", "10")]
    if d >= LATE_START:
        rows.append(("LATE", "7", str(10 + k % 4)))
    rows += [("ZZZ", "5.5", "10.25"), ("UNK", "1", "1"), ("GCVr", "1", "2"), ("GTXw", "1", "2")]
    return rows


def write_cnms(root, override=None):
    """CNMSshvol files (CRLF, row-count trailer, gz) and the downloader receipt manifest.csv."""
    root.mkdir(parents=True)
    receipt = ["date,file,bytes,sha256_of_raw_bytes,rows,url,http_status,downloaded_at",
               "2024-03-29,,0,,0,https://x/CNMSshvol20240329.txt,403,2026-09-27T23:00:00+00:00"]
    for k, d in enumerate(sorted(file_days())):
        rows = cnms_rows(d, k)
        if override:
            rows = override(d, rows)
        ymd = d.strftime("%Y%m%d")
        text = "Date|Symbol|ShortVolume|ShortExemptVolume|TotalVolume|Market\r\n"
        text += "".join(f"{ymd}|{s}|{sv}|0|{tv}|Q,N\r\n" for s, sv, tv in rows) + f"{len(rows)}\r\n"
        raw = text.encode()
        name = f"CNMSshvol{ymd}.txt.gz"
        (root / name).write_bytes(gzip.compress(raw, mtime=0))
        receipt.append(f"{d.isoformat()},{name},{len(raw)},{hashlib.sha256(raw).hexdigest()},{len(rows)},"
                       f"https://x/{name[:-3]},200,2026-09-27T23:{k // 60:02d}:{k % 60:02d}+00:00")
    (root / "manifest.csv").write_text("\n".join(receipt) + "\n", encoding="utf-8")


class SvFixture(base.Fixture):
    def __init__(self, root: Path):
        self.base = root
        self.th = root / "TickerHistory3.parquet"
        base.write_tickerhistory(self.th)
        add_tickers(self.th)
        self.finra = root / "finra"
        base.write_finra(self.finra)
        self.lake = root / "lake"
        base.write_lake(self.lake)
        self.role = root / "role"
        self.role_sha = base.write_role(self.role, base.sha(self.th))
        self.sv = root / "cnms"
        write_cnms(self.sv)

    def run(self, out, **kw):
        kw.setdefault("finra_short_volume", self.sv)
        return super().run(out, **kw)


def reference(fx: SvFixture, sv_dir: Path):
    """Brute force with the producer's canon / build_maps and its collision loop (transcribed from main())."""
    prod = load_producer()
    th = pq.read_table(fx.th, columns=["tradingDate", "securityID", "ticker_tk"]).to_pandas()
    th["ticker_tk"] = th["ticker_tk"].fillna("")
    th = th[(th["securityID"] > 0) & (th["ticker_tk"].str.strip() != "")].copy()
    th["tradingDate"] = th["tradingDate"].astype(str)
    files = {}
    for path in sorted(sv_dir.glob("CNMSshvol*.txt.gz")):
        d = dt.datetime.strptime(path.name[9:17], "%Y%m%d").date()
        lines = gzip.decompress(path.read_bytes()).decode().split("\r\n")[1:-2]
        last = {}
        for line in lines:  # within a file the last row of a symbol wins
            _, sym, sv, _, tv, _ = line.split("|")
            last[sym] = (float(sv), float(tv))
        files[d] = last
    maps = prod.build_maps(th, sorted(d.isoformat() for d in files))
    mapped = {}
    for d, rows in files.items():
        m = maps[d.isoformat()]
        if m is None:
            continue
        cand = {}
        for sym, vals in rows.items():
            spelled = sym.replace("p", "PR").replace("r", "RT").replace("w", "WI")  # CNMS -> short-interest spelling
            c = prod.canon(spelled)
            if c in m[3] or c not in m[1]:
                continue
            cand.setdefault(m[1][c], []).append((spelled.strip().upper() in m[2][c], vals))
        out = {}
        for sid, lst in cand.items():
            if len(lst) > 1:
                ex = [x for x in lst if x[0]]
                if len(ex) != 1:
                    continue
                lst = ex
            out[sid] = lst[0][1]
        mapped[d] = out
    prefix = sorted(d for d in files if d < SESSIONS[0])[-126:]
    calendar = prefix + list(SESSIONS)
    ref = np.full((len(SESSIONS), len(IDS)), np.nan)
    count = np.zeros((len(SESSIONS), len(IDS)), dtype=int)
    for t, d in enumerate(SESSIONS):
        e = len(prefix) + t
        window = calendar[max(0, e - 126):e]
        for i, sid in enumerate(IDS):
            obs = [mapped[s][sid] for s in window if s in mapped and sid in mapped[s]]
            count[t, i] = len(obs)
            total = sum(x[1] for x in obs)
            if len(obs) >= 63 and total > 0:
                ref[t, i] = sum(x[0] for x in obs) / total
    return ref, count, mapped


class ShortVolumeField(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = SvFixture(Path(cls.temp.name))
        cls.manifest = cls.fx.run("sv", fields=["sv_ratio126"])
        cls.sv = cls.fx.field("sv", "sv_ratio126")
        cls.ref, cls.count, cls.mapped = reference(cls.fx, cls.fx.sv)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def entry(self):
        return next(e for e in self.manifest["fields"] if e["name"] == "sv_ratio126")

    def test_equals_the_reference_built_on_the_producer_map(self):
        np.testing.assert_array_equal(self.sv, self.ref)
        self.assertTrue(np.isfinite(self.sv).sum() > 100)
        for i in range(len(IDS)):
            self.assertTrue(np.isfinite(self.sv[:, i]).any(), IDS[i])

    def test_ratio_formula_by_hand_for_one_cell(self):
        t, i = base.t_of("2024-12-02"), IDS.index(101)
        days = sorted(self.mapped)
        prefix = [d for d in days if d < SESSIONS[0]][-126:]
        window = (prefix + list(SESSIONS))[len(prefix) + t - 126:len(prefix) + t]
        self.assertEqual(len(window), 126)
        self.assertNotIn(SESSIONS[t], window)                      # lag 1: the session's own file is excluded
        obs = [self.mapped[s][101] for s in window if s in self.mapped and 101 in self.mapped[s]]
        self.assertEqual(len(obs), 126 - 1 - 5)                    # 10-15 has no file; AAA ambiguous 08-05..08-09
        self.assertEqual(self.sv[t, i], sum(x[0] for x in obs) / sum(x[1] for x in obs))

    def test_ambiguity_collision_duplicate_and_translation(self):
        amb = [d for d in self.mapped if AMB[0] <= d <= AMB[1]]
        self.assertEqual(len(amb), 5)
        for d in amb:
            self.assertNotIn(101, self.mapped[d])                  # AAA also spelled by 999 that day: dropped
        wed = next(d for d in self.mapped if d.weekday() == 2 and d > AMB[1])
        self.assertEqual(self.mapped[wed][101][0], 100 + sorted(file_days()).index(wed) % 17)  # exact AAA kept
        self.assertEqual(self.mapped[DUP_DAY][101][1], 1000 + (sorted(file_days()).index(DUP_DAY) % 5) * 10)
        self.assertEqual(self.mapped[Z_CPK][303], (20.0 + sorted(file_days()).index(Z_CPK) % 3, 100.0))  # CpK -> CPRK
        self.assertNotIn(202, self.mapped[BLANK[0]])
        self.assertNotIn(505, self.mapped[NULL[0]])
        m = self.entry()["short_volume"]["mapping"]
        totals = m["totals"]
        wednesdays = sum(1 for d in read_days() if d.weekday() == 2)
        self.assertEqual(totals["rows_ambiguous"], 5 + 1)          # AAA x 5 days + AA/A on Wednesday 08-07
        self.assertEqual(totals["rows_collision_dropped"], wednesdays - 1)
        self.assertEqual(totals["duplicate_symbol_rows"], 1)
        self.assertEqual(totals["rows_unknown_lowercase_marker"], 0)
        self.assertGreater(totals["rows_kept_canonical_only"], 0)  # BF/B -> BF.B
        self.assertEqual(m["tickerhistory"]["ambiguous_role_keys"], 5)

    def test_pit_ticker_change_uses_the_file_date(self):
        self.assertEqual(self.mapped[X_NEW - dt.timedelta(days=3)][404], (1.0, 10.0))  # Friday: OLD
        self.assertEqual(self.mapped[X_NEW][404], (9.0, 10.0))                        # Monday: NEW
        t = base.t_of("2024-10-01")
        window = [d for d in sorted(self.mapped) if d < SESSIONS[t]][-126:]
        new = sum(1 for d in window if d >= X_NEW)
        self.assertEqual(self.sv[t, IDS.index(404)], (new * 9 + (126 - new)) / (126 * 10))

    def test_min_sessions_and_zero_total(self):
        late = IDS.index(505)
        for t in range(len(SESSIONS)):
            self.assertEqual(np.isfinite(self.sv[t, late]), self.count[t, late] >= 63, t)
        first = int(np.argmax(np.isfinite(self.sv[:, late])))
        self.assertTrue(0 < first < len(SESSIONS) - 1 and self.count[first, late] == 63 and self.count[first - 1, late] == 62)
        cpk = IDS.index(303)
        z = SESSIONS.index(Z_CPK)
        self.assertTrue((self.count[:, cpk] >= 63).all())
        self.assertTrue(np.isnan(self.sv[:z + 1, cpk]).all())      # every window row has TotalVolume 0
        self.assertTrue(np.isfinite(self.sv[z + 1:, cpk]).all())   # the 10-16 file enters at 10-17

    def test_manifest_records_sources_formula_and_coverage(self):
        e = self.entry()
        self.assertIs(e["point_in_time"], True)
        self.assertEqual(e["non_pit_aspects"], [])
        sv = e["short_volume"]
        self.assertEqual(sv["formula_id"], "finra-cnms-ratio126-lag1-v1")
        self.assertEqual((sv["window_sessions"], sv["min_sessions"], sv["lag_sessions"]), (126, 63, 1))
        read = read_days()
        listing = [[f"CNMSshvol{d:%Y%m%d}.txt.gz", (self.fx.sv / f"CNMSshvol{d:%Y%m%d}.txt.gz").stat().st_size,
                    base.sha(self.fx.sv / f"CNMSshvol{d:%Y%m%d}.txt.gz")] for d in read]
        digest = hashlib.sha256(json.dumps(sorted(listing), sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        f = sv["files"]
        self.assertEqual((f["files_read"], f["files_sha256"]), (len(read), digest))
        self.assertEqual((f["first_file_date"], f["last_file_date"]), (read[0].isoformat(), read[-1].isoformat()))
        self.assertEqual(f["path"], str(self.fx.sv.resolve()))
        self.assertEqual(f["receipt"]["sha256"], base.sha(self.fx.sv / "manifest.csv"))
        cal = sv["calendar"]
        self.assertEqual(cal["prefix_sessions"], 126)
        self.assertEqual((cal["role_sessions_without_file"], cal["role_sessions_without_file_first"]),
                         (1, [MISSING.isoformat()]))
        self.assertEqual((cal["files_off_role_calendar"], cal["files_off_role_calendar_first"]),
                         (1, [WEEKEND.isoformat()]))
        self.assertEqual(e["sources"][0]["files_sha256"], digest)
        self.assertEqual(e["sources"][2]["sha256"], base.sha(self.fx.th))
        cov = e["coverage"]
        self.assertEqual(list(cov["per_year"]), ["2024"])
        member = np.fromfile(self.fx.role / "member.u8", dtype="u1").reshape(len(SESSIONS), len(IDS)) != 0
        self.assertEqual(cov["per_year"]["2024"]["finite_member_cells"], int((member & np.isfinite(self.sv)).sum()))
        self.assertEqual(self.manifest["source_checks"]["finra_short_volume"]["files_sha256"], digest)

    def test_lag_the_sessions_own_file_is_never_used(self):
        k = 30
        target = SESSIONS[k]
        lagged = self.fx.base / "cnms-lag"
        write_cnms(lagged, override=lambda d, rows: [("AAA", "900", "1000") if s == "AAA" and d == target else
                                                     (s, a, b) for s, a, b in rows])
        self.fx.run("sv-lag-out", fields=["sv_ratio126"], finra_short_volume=lagged)
        got = self.fx.field("sv-lag-out", "sv_ratio126")
        np.testing.assert_array_equal(got[:k + 1], self.sv[:k + 1])   # rows <= the file's own session unchanged
        self.assertGreater(got[k + 1, IDS.index(101)], self.sv[k + 1, IDS.index(101)])
        np.testing.assert_array_equal(np.delete(got, IDS.index(101), axis=1), np.delete(self.sv, IDS.index(101), axis=1))

    def test_receipt_and_contract_refusals(self):
        bad = self.fx.base / "cnms-tampered"
        shutil.copytree(self.fx.sv, bad)
        name = f"CNMSshvol{SESSIONS[5]:%Y%m%d}.txt.gz"
        raw = gzip.decompress((bad / name).read_bytes()).replace(b"|AAA|", b"|AAB|")
        (bad / name).write_bytes(gzip.compress(raw, mtime=0))
        with self.assertRaisesRegex(ValueError, "does not match its manifest.csv row"):
            self.fx.run("tampered-out", fields=["sv_ratio126"], finra_short_volume=bad)
        with self.assertRaisesRegex(ValueError, "needs --finra-short-volume"):
            self.fx.run("noarg-out", fields=["sv_ratio126"], finra_short_volume=None)
        day = tool.day_of(dt.date(2024, 5, 1))
        good = b"Date|Symbol|ShortVolume|ShortExemptVolume|TotalVolume|Market\r\n20240501|A|1|0|2|Q\r\n1\r\n"
        self.assertEqual(tool.parse_cnms(good, day, "x")[0], ["A"])
        for blob, why in ((good.replace(b"\r\n1\r\n", b"\r\n2\r\n"), "trailer"),
                          (good.replace(b"20240501|", b"20240502|"), "Date"),
                          (good.replace(b"|1|0|2|", b"|3|0|2|"), "ShortVolume above"),
                          (good.replace(b"|1|0|2|", b"|-1|0|2|"), "non-negative decimal"),
                          (good.replace(b"|A|", b"|A B|"), "Symbol"),
                          (good.replace(b"Market", b"Mkt"), "header"),
                          (good.replace(b"\r\n1\r\n", b"\r\n"), "trailer")):
            with self.assertRaisesRegex(ValueError, why):
                tool.parse_cnms(blob, day, "x")


class SymbolMapReuse(unittest.TestCase):
    """The builder's port of the short-interest producer's map is the producer's map."""

    def test_canonicaliser_and_spelling_match_the_producer(self):
        prod = load_producer()
        self.assertEqual(tool.SI_CANON_PATTERN, prod._CANON_RE.pattern)
        self.assertEqual(tool.SV_TICKER_LOOKBACK_DAYS, prod.LOOKBACK_DAYS)
        for t in ("BF.B", "BF/B", "brk-b", " AAPL ", "C.PRK", "ACP.RTWI", "A AA", "abc.d/e-f", "ABRpA", "X\tY"):
            self.assertEqual(tool.si_canon(t), prod.canon(t), t)
        cases = {"CpK": "CPRK", "BF/B": "BFB", "GCVr": "GCVRT", "GTXw": "GTXWI", "FTFrw": "FTFRTWI",
                 "BACpD/CL": "BACPRDCL", "AIG/WS": "AIGWS", "AAPL": "AAPL"}
        for sym, want in cases.items():
            self.assertEqual(tool.si_canon(tool.cnms_to_si(sym)), want, sym)
        # the ORATS spellings of the same lines canonicalise to the same keys
        for orats, sym in (("C.PRK", "CpK"), ("BF.B", "BF/B"), ("ACP.RTWI", "ACPrw"), ("AAN.WI", "AANw")):
            self.assertEqual(prod.canon(orats), tool.si_canon(tool.cnms_to_si(sym)), orats)
        self.assertNotEqual(tool.si_canon(tool.cnms_to_si("CpK")), prod.canon("CPK"))  # no collision with CPK

    def test_collision_rule(self):
        col = np.array([3, 3, 4, 5, 5, 6, 6, -1, -2])
        exact = np.array([True, False, False, True, True, False, False, False, False])
        keep = tool.sv_resolve_collisions(col, exact)
        self.assertEqual(keep.tolist(), [True, False, True, False, False, False, False, False, False])


class ByteIdentity(unittest.TestCase):
    """Every other field's bytes and manifest entry are identical with and without sv_ratio126."""

    def test_full_recipe_with_and_without_sv(self):
        with tempfile.TemporaryDirectory() as temp:
            fx = SvFixture(Path(temp))
            bridge, events = fx.base / "bridge", fx.base / "events"
            kw = dict(identity_bridge=bridge, identity_bridge_sha256=base.write_bridge(bridge),
                      fund_events=events, fund_events_sha256=base.write_events(events))
            recipe = list(tool.FIELDS) + list(tool.ISSUER_FIELDS)
            without = fx.run("without", fields=recipe, finra_short_volume=None, **kw)
            with_sv = fx.run("with", fields=recipe + ["sv_ratio126"], **kw)
            self.assertEqual([e["name"] for e in with_sv["fields"]], recipe + ["sv_ratio126"])
            for name in recipe:
                a, b = (fx.base / "without" / f"{name}.f64").read_bytes(), (fx.base / "with" / f"{name}.f64").read_bytes()
                self.assertEqual(a, b, name)
                self.assertEqual(without["files"][f"{name}.f64"], with_sv["files"][f"{name}.f64"], name)
                ea = next(e for e in without["fields"] if e["name"] == name)
                eb = next(e for e in with_sv["fields"] if e["name"] == name)
                self.assertEqual(ea, eb, name)
            for key, value in without["source_checks"].items():
                self.assertEqual(with_sv["source_checks"][key], value, key)
            self.assertEqual(set(with_sv["source_checks"]) - set(without["source_checks"]), {"finra_short_volume"})
            # the sv field is the same whether or not the TickerHistory group ran first (digest reuse path)
            alone = fx.run("alone", fields=["sv_ratio126"])
            self.assertEqual(alone["files"]["sv_ratio126.f64"], with_sv["files"]["sv_ratio126.f64"])
        self.assertEqual(tool.DEFAULT_FIELDS, ("si_shares", "si_dtc", "iv_atm_21d", "iv_atm_63d", "iv_atm_126d",
                                               "earn_recent", "shares_out", "mkt_ret"))
        self.assertNotIn("sv_ratio126", tool.FIELDS)
        self.assertLessEqual(len(recipe) + 1, 64)  # runner field limit


if __name__ == "__main__":
    unittest.main()
