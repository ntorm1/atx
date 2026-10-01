"""research_fields_v9 ``nt_first_126`` (library v9 draft C-3 nt_late, spec F-L1; lane FIELDS-V9): synthetic stages only.

A role of the NYSE sessions 2022-07-01 .. 2023-06-30 (14 lines), a SEC identity bridge and an atx-db-like sec_filings
stage (events.parquet with NT 10-K / NT 10-Q / NT 20-F rows, 8-K item and Form 25 noise; filings.parquet with the
periodic forms of domestic filers, a 20-F filer, amendments and noise; eight_k_items.parquet for the 8-K fields). Every
cell is re-derived by a brute-force oracle from the definition (its own hand-listed NYSE calendar, datetime visibility
tests, set-based 'first' lookback). Planted: a notice accepted 22:30 UTC (usable two sessions later) and one at 21:59
UTC (the next session); a notice exactly 365 days after a prior one (not first) and 365 days + 1 microsecond after
(first); a prior NT 20-F that makes an NT 10-K not first; an NT amendment (ignored); a duplicated accession with two
clocks (the later); a 20-F filer (NaN); a periodic amendment that must not extend the 400-day presence; a link that
starts inside the role; a J line and an unlinked line; rows after the role and after the seal. The draft module is
registered only inside this file's tests (prepare_research_fields_draft.register, undone on exit).
"""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np
import pyarrow as pa

import prepare_research_fields as tool
import prepare_research_fields_draft as draft
import research_fields_sec as sec
import research_fields_v9 as v9
import research_window as rw
import test_prepare_research_fields as base
import test_prepare_research_fields_sec as sect
from test_research_window import isolated

D = dt.date.fromisoformat
at, mark, naive = base.at, base.mark, sect.naive
TOOLS = Path(__file__).resolve().parent
NAME = "nt_first_126"
K8_RUN = ["k8_count_63", "k8_days_since_any"]

HOLIDAYS = {D(x) for x in (
    "2021-01-01", "2021-01-18", "2021-02-15", "2021-04-02", "2021-05-31", "2021-07-05", "2021-09-06", "2021-11-25",
    "2021-12-24", "2022-01-17", "2022-02-21", "2022-04-15", "2022-05-30", "2022-06-20", "2022-07-04", "2022-09-05",
    "2022-11-24", "2022-12-26", "2023-01-02", "2023-01-16", "2023-02-20", "2023-04-07", "2023-05-29", "2023-06-19",
    "2023-07-04", "2023-09-04", "2023-11-23", "2023-12-25")}
CAL = [d for d in (D("2021-01-01") + dt.timedelta(days=i) for i in range(1095))
       if d.weekday() < 5 and d not in HOLIDAYS]                                  # 2021-01-04 .. 2023-12-29
IDX = {d: i for i, d in enumerate(CAL)}
SESSIONS = [d for d in CAL if D("2022-07-01") <= d <= D("2023-06-30")]
IDS = list(range(101, 115))
CUT = SESSIONS.index(D("2023-03-01"))
LONG_AGO = base.LONG_AGO

# (sr_id, cik, start, end_incl, available_at, primary, tier, basis)
BRIDGE = [(sid, cik, D("2015-01-01"), None, LONG_AGO, "P", "strict", "reconstructed_high")
          for sid, cik in ((101, 1001), (103, 1003), (104, 1004), (105, 1005), (106, 1006), (109, 1009), (110, 1010),
                           (111, 1011), (112, 1012), (113, 1013), (114, 1014))] + [
    (102, 1001, D("2015-01-01"), None, LONG_AGO, "J", "strict", "reconstructed_high"),      # second class: NaN
    (107, 1007, D("2022-11-01"), None, mark(D("2022-11-01")), "P", "name", "finra_name_match"),   # starts in the role
    (999, 9999, D("2015-01-01"), None, LONG_AGO, "P", "strict", "reconstructed_high")]      # off the role axis
DOMESTIC = (1001, 1003, 1004, 1007, 1009, 1010, 1011, 1012, 1013, 1014)


def acc(cik, when, seq):
    return f"{cik:010d}-{when.year % 100:02d}-{seq:06d}"


# ---- events.parquet: (cik, form, available_at, is_amendment, event_type, item)
NT = [
    (1001, "NT 10-Q", at(2022, 8, 15, 15)),               # first: usable from 08-16
    (1001, "NT 10-K", at(2023, 3, 31, 20)),               # within 365 days of the first: not first
    (1003, "NT 10-Q", at(2021, 8, 10, 14)),               # first, before the role
    (1003, "NT 10-K", at(2022, 9, 28, 22, 30)),           # 414 days later: first; 22:30 UTC -> usable 09-30, not 09-29
    (1004, "NT 10-K", at(2021, 11, 1, 14)),
    (1004, "NT 10-Q", at(2022, 8, 10, 14)),               # not first
    (1004, "NT 10-Q", at(2023, 1, 5, 14)),                # not first
    (1005, "NT 20-F", at(2022, 10, 3, 14)),               # a 20-F filer: NaN
    (1006, "NT 10-K", at(2022, 12, 1, 13)),               # first
    (1007, "NT 10-Q", at(2022, 10, 20, 12)),              # first; the line links from 11-01
    (1009, "NT 10-Q", at(2023, 2, 14, 21, 59)),           # first; 21:59 UTC -> usable 02-15
    (1009, "NT 10-K", at(2023, 8, 1, 14)),                # after the role
    (1009, "NT 10-K", at(2024, 2, 1, 14)),                # after the role (sealed under the repository window)
    (1009, "NT 10-Q", at(2025, 3, 3, 14)),                # sealed (the window conftest.py binds)
    (1010, "NT 10-K", at(2021, 12, 1, 15)),
    (1010, "NT 10-Q", at(2022, 12, 1, 15)),               # exactly 365 days later: not first
    (1010, "NT 10-K", at(2023, 3, 15, 15)),
    (1011, "NT 10-K", at(2021, 12, 1, 15)),
    (1011, "NT 10-Q", at(2022, 12, 1, 15, 0, 1)),         # 365 days + 1 microsecond later: first
    (1012, "NT 20-F", at(2022, 7, 15, 14)),
    (1012, "NT 10-K", at(2023, 1, 10, 14)),               # a prior NT 20-F within 365 days: not first
    (7777, "NT 10-Q", at(2022, 9, 1, 14)),                # unlinked CIK
]
NT_EXTRA = [  # (cik, accession, form, available_at, is_amendment)
    (1006, acc(1006, at(2022, 9, 1), 900), "NT 10-K/A", at(2022, 9, 1, 14), True),      # amendment: ignored
    (1013, acc(1013, at(2023, 1, 9), 901), "NT 10-Q", at(2023, 1, 9, 15), False),       # one accession, two clocks:
    (1013, acc(1013, at(2023, 1, 9), 901), "NT 10-Q", at(2023, 1, 10, 23), False),      # the later -> usable 01-12
]
NOISE = [  # (cik, form, available_at, event_type, item)
    (1001, "8-K", at(2022, 8, 1, 20), "earnings_release", "2.02"),
    (1003, "8-K", at(2022, 10, 3, 14), "material_agreement", "1.01"),
    (1004, "25", at(2023, 2, 1, 14), "form25_delisting", None),
]
EVENT_TYPE = {"NT 10-K": "late_filing_nt_10k", "NT 10-Q": "late_filing_nt_10q", "NT 20-F": "late_filing_nt_20f"}


def event_rows(nt=NT, extra=NT_EXTRA, noise=NOISE):
    rows = [(c, acc(c, a, 100 + i), f, a, False, EVENT_TYPE[f], None, "form") for i, (c, f, a) in enumerate(nt)]
    rows += [(c, s, f, a, amend, EVENT_TYPE[f.replace("/A", "")], None, "form") for c, s, f, a, amend in extra]
    rows += [(c, acc(c, a, 500 + i), f, a, False, e, it, "eight_k_item" if it else "form")
             for i, (c, f, a, e, it) in enumerate(noise)]
    return rows


def events_table(rows):
    c = list(zip(*rows))
    return pa.table({"cik": pa.array(c[0], pa.int64()), "accession": pa.array(c[1], pa.string()),
                     "form": pa.array(c[2], pa.string()), "event_type": pa.array(c[5], pa.string()),
                     "item": pa.array(c[6], pa.string()), "source": pa.array(c[7], pa.string()),
                     "available_at": pa.array([naive(x) for x in c[3]], pa.timestamp("us")),
                     "filing_date": pa.array([x.date() for x in c[3]], pa.date32()),
                     "is_amendment": pa.array(c[4], pa.bool_())})


# ---- filings.parquet: (cik, form, available_at, is_amendment)
def filing_rows():
    rows = []
    for cik in DOMESTIC:
        for y in (2021, 2022, 2023):
            for (m, d), form in (((3, 1), "10-K"), ((5, 10), "10-Q"), ((8, 9), "10-Q"), ((11, 8), "10-Q")):
                if cik == 1012 and (y, form) == (2022, "10-K"):
                    form = "10-KT"                                       # a transition report: periodic too
                rows.append((cik, form, at(y, m, d, 20), False))
    rows += [(1001, "10-K", at(2024, 3, 1, 20), False), (1009, "10-K", at(2024, 3, 1, 20), False),
             (1001, "10-Q", at(2025, 5, 12, 20), False)]                 # after the role / sealed
    rows += [(1006, f, a, False) for f, a in (("10-Q", at(2021, 8, 9, 20)), ("10-Q", at(2021, 11, 8, 20)),
                                              ("10-K", at(2022, 3, 1, 20)), ("10-Q", at(2022, 5, 16, 14)))]
    rows += [(1006, "10-Q/A", at(2023, 1, 17, 15), True)]                # an amendment never extends presence
    rows += [(1005, "20-F", at(2021, 4, 30, 20), False), (1005, "20-F", at(2022, 4, 29, 20), False),
             (1005, "6-K", at(2022, 8, 1, 20), False)]
    rows += [(1001, "8-K", at(2022, 8, 1, 20), False), (1001, "4", at(2022, 9, 1, 20), False),
             (1014, "S-8", at(2023, 2, 1, 20), False)]
    return rows


def filings_table(rows):
    c = list(zip(*rows))
    return pa.table({"cik": pa.array(c[0], pa.int64()),
                     "accession": pa.array([acc(x, a, 2000 + i) for i, (x, a) in enumerate(zip(c[0], c[2]))],
                                           pa.string()),
                     "form": pa.array(c[1], pa.string()),
                     "filing_date": pa.array([x.date() for x in c[2]], pa.date32()),
                     "acceptance_utc": pa.array([naive(x) for x in c[2]], pa.timestamp("us")),
                     "available_at": pa.array([naive(x) for x in c[2]], pa.timestamp("us")),
                     "is_amendment": pa.array(c[3], pa.bool_())})


K8 = [(1001, "0000001001-22-000700", "8-K", ("2.02", "9.01"), at(2022, 8, 1, 20)),
      (1003, "0000001003-22-000701", "8-K", ("1.01",), at(2022, 10, 3, 14)),
      (1004, "0000001004-23-000702", "8-K", ("5.02",), at(2023, 2, 1, 14)),
      (1009, "0000001009-23-000703", "8-K", ("8.01",), at(2023, 5, 1, 14))]


def write_role(root: Path) -> str:
    root.mkdir(parents=True)
    member = np.ones((len(SESSIONS), len(IDS)), dtype="u1")
    member[0:10, IDS.index(114)] = 0
    blobs = {"sessions.i64": np.array([(d - tool.EPOCH).days * tool.DAY_NS for d in SESSIONS], dtype="<i8").tobytes(),
             "ids.u64": np.array(IDS, dtype="<u8").tobytes(), "member.u8": member.tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    manifest = {"schema": tool.ROLE_SCHEMA, "status": "complete", "dates": len(SESSIONS), "instruments": len(IDS),
                "instrument_namespace": "spiderrock.securityID", "score_begin": 0, "score_end": len(SESSIONS),
                "files": files}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return base.sha(root / "manifest.json")


class World:
    """The role, the SEC bridge and the sec_filings stage (``events`` / ``filings``: rows to write)."""

    def __init__(self, root: Path, events=None, filings=None, name="w"):
        self.base = root
        self.events = event_rows() if events is None else events
        self.filings = filing_rows() if filings is None else filings
        self.role = root / "role"
        self.role_sha = write_role(self.role) if not self.role.exists() else base.sha(self.role / "manifest.json")
        self.bridge = root / "sec_bridge"
        self.bridge_sha = base.write_bridge(self.bridge, rows=BRIDGE) if not self.bridge.exists() else \
            base.sha(self.bridge / "manifest.json")
        self.stages = root / f"{name}-stages"
        self.stage_sha = sect.write_stage(self.stages / "sec_filings", sec.STAGES["sec_filings"][1],
                                          {"events.parquet": events_table(self.events),
                                           "filings.parquet": filings_table(self.filings),
                                           "eight_k_items.parquet": sect.eightk_table(K8)})

    def options(self, **over):
        return {"sec_stages": self.stages, "sec_identity_bridge": self.bridge,
                "sec_identity_bridge_sha256": self.bridge_sha, "sec_filings_sha256": self.stage_sha, **over}

    def run(self, out, fields=(NAME,), **kw):
        kw.setdefault("module_options", self.options())
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(self.role, self.role_sha, self.base / out, list(fields), **kw)

    def field(self, out, name=NAME):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(SESSIONS), len(IDS))


@contextlib.contextmanager
def drafted():
    """The builder with the draft modules registered; the registry and module list are restored on exit, so every
    other test module sees the plain builder."""
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        draft.register(vars(tool))
        yield


# ---- the oracle ----------------------------------------------------------------------------------------------------
def visible(avail, d, seal):
    """The SEC clock: available_at < 22:00 UTC of the session before d (and before the seal)."""
    return avail < seal and avail < mark(CAL[IDX[d] - 1])


def usable_index(avail):
    """The first calendar index whose previous session's mark follows ``avail`` (after the hand-listed calendar: it
    can flag no role session)."""
    return next((i for i in range(1, len(CAL)) if avail < mark(CAL[i - 1])), len(CAL) + 1000)


BOUND_SEAL = dt.datetime.combine(rw.SEAL, dt.time(0), tzinfo=dt.timezone.utc)   # the window the builder bound


def link_of(sid, d):
    q = [(c, k) for s, c, start, end, avail, k, _, _ in BRIDGE
         if s == sid and start <= d and (end is None or d <= end) and avail <= mark(d)]
    return q[0] if q else (None, None)


def oracle(events, filings, seal=BOUND_SEAL):
    out = np.full((len(SESSIONS), len(IDS)), np.nan)
    notices = {}
    for c, a_, f, avail, amend, *_ in events:
        if f in ("NT 10-K", "NT 10-Q", "NT 20-F") and not amend and avail < seal:
            notices[(c, a_)] = (f, max(avail, notices.get((c, a_), (f, avail))[1]))
    first = {}
    for (c, a_), (f, avail) in notices.items():
        prior = [v for (c2, a2), (_, v) in notices.items() if c2 == c and avail - dt.timedelta(days=365) <= v < avail]
        if not prior and f != "NT 20-F":
            first.setdefault(c, []).append(usable_index(avail))
    for t, d in enumerate(SESSIONS):
        cut = mark(CAL[IDX[d] - 1])
        for i, sid in enumerate(IDS):
            cik, kind = link_of(sid, d)
            if kind != "P":
                continue
            present = any(c == cik and f in v9.PERIODIC_FORMS and not amend and visible(a, d, seal)
                          and a >= cut - dt.timedelta(days=400) for c, f, a, amend in filings)
            if present:
                out[t, i] = float(any(u <= IDX[d] < u + 126 for u in first.get(cik, [])))
    return out


def entry(manifest, name=NAME):
    return next(e for e in manifest["fields"] if e["name"] == name)


class NtFirst(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stack = contextlib.ExitStack()
        cls.stack.enter_context(drafted())
        cls.temp = cls.stack.enter_context(tempfile.TemporaryDirectory())
        cls.w = World(Path(cls.temp))
        cls.manifest = cls.w.run("nt")
        cls.got = cls.w.field("nt")

    @classmethod
    def tearDownClass(cls):
        cls.stack.close()

    def at_(self, day, sid):
        return self.got[SESSIONS.index(D(day)), IDS.index(sid)]

    def test_calendar_is_the_rule(self):
        got = sec.nyse_sessions(D("2021-01-01"), D("2023-12-31"))
        self.assertEqual(got.tolist(), [(d - tool.EPOCH).days for d in CAL])

    def test_values_equal_the_definition(self):
        np.testing.assert_array_equal(self.got, oracle(self.w.events, self.w.filings))
        self.assertGreater(int(np.nansum(self.got)), 300)                    # the fixture flags
        self.assertGreater(int((self.got == 0).sum()), 1000)                 # ... and has present, unflagged cells
        for sid in (102, 105, 108):                                          # J line, 20-F filer, unlinked line
            self.assertTrue(np.isnan(self.got[:, IDS.index(sid)]).all(), sid)
        for sid in (104, 110, 112, 114):                                     # never a first notice in the role
            self.assertTrue((self.got[:, IDS.index(sid)] == 0).all(), sid)

    def test_hand_computed_cells(self):
        # 1001: NT 10-Q 2022-08-15 15:00 UTC -> event session 08-15, usable 08-16 for 126 sessions (to 2023-02-14);
        # its NT 10-K of 2023-03-31 is within 365 days: not first, no new window
        self.assertEqual(self.at_("2022-08-15", 101), 0.0)
        self.assertEqual(self.at_("2022-08-16", 101), 1.0)
        last = SESSIONS[SESSIONS.index(D("2022-08-16")) + 125]
        self.assertEqual(last, D("2023-02-14"))
        self.assertEqual(self.at_("2023-02-14", 101), 1.0)
        self.assertEqual(self.at_("2023-02-15", 101), 0.0)
        self.assertEqual(self.at_("2023-04-04", 101), 0.0)
        # 1003: accepted 2022-09-28 22:30 UTC: the 22:00 mark of 09-28 precedes it -> event 09-29, usable 09-30
        self.assertEqual(self.at_("2022-09-29", 103), 0.0)
        self.assertEqual(self.at_("2022-09-30", 103), 1.0)
        # 1009: accepted 2023-02-14 21:59 UTC -> usable 02-15
        self.assertEqual(self.at_("2023-02-14", 109), 0.0)
        self.assertEqual(self.at_("2023-02-15", 109), 1.0)
        # 1010: a notice exactly 365 days after a prior one is not first; 1011: 365 days + 1 us later is first
        self.assertEqual(self.at_("2022-12-02", 110), 0.0)
        self.assertEqual(self.at_("2022-12-01", 111), 0.0)
        self.assertEqual(self.at_("2022-12-02", 111), 1.0)
        # 1012: an NT 20-F 179 days before its NT 10-K makes the NT 10-K not first
        self.assertEqual(self.at_("2023-01-12", 112), 0.0)
        # 1013: one accession with two clocks takes the later (2023-01-10 23:00 UTC) -> usable 01-12
        self.assertEqual(self.at_("2023-01-11", 113), 0.0)
        self.assertEqual(self.at_("2023-01-12", 113), 1.0)
        # 1007: first notice 2022-10-20 (usable 10-21); the line links from 2022-11-01
        self.assertTrue(math.isnan(self.at_("2022-10-31", 107)))
        self.assertEqual(self.at_("2022-11-01", 107), 1.0)
        # 1006: first NT 10-K 2022-12-01 13:00 UTC -> usable 12-02 for 126 sessions (to 2023-06-05); its NT 10-K/A
        # of 2022-09-01 is ignored. Last original periodic filing 2022-05-16 14:00 UTC: present while it is within
        # 400 days of the 22:00 UTC mark of t-1 (to 2023-06-20; 06-19 is a holiday); its 10-Q/A of 2023-01-17 does
        # not extend that
        self.assertEqual(self.at_("2022-09-02", 106), 0.0)
        self.assertEqual(self.at_("2022-12-02", 106), 1.0)
        self.assertEqual(SESSIONS[SESSIONS.index(D("2022-12-02")) + 125], D("2023-06-05"))
        self.assertEqual(self.at_("2023-06-05", 106), 1.0)
        self.assertEqual(self.at_("2023-06-06", 106), 0.0)
        self.assertEqual(self.at_("2023-06-20", 106), 0.0)
        self.assertTrue(math.isnan(self.at_("2023-06-21", 106)))

    def test_manifest_entry_and_checks(self):
        e = entry(self.manifest)
        spec = v9.FIELDS[NAME]
        self.assertEqual((e["formula_id"], e["clock"], e["lag_sessions"], e["point_in_time"]),
                         ("sec-nt-first365-126-v1", sec.SEC_CLOCK, 1, True))
        self.assertEqual(e["formula_sha256"], tool.formula_id(NAME, tool.spec_definition(NAME, 0)))
        self.assertEqual(e["producer"]["module"], "research_fields_v9.py")
        self.assertEqual(e["stage_manifests"]["sec_filings"]["sha256"], self.w.stage_sha)
        self.assertEqual(e["identity_bridge_manifest_sha256"], self.w.bridge_sha)
        self.assertEqual(e["imported_code"], v9.imported_code("v9_nt"))
        self.assertEqual(v9.entry_inputs(e), v9.reuse_inputs(NAME, self.w.options()))
        self.assertEqual(set(e["nan_reasons_member_cells"]), {"not_primary_link", "absent_or_stale", "out_of_rule"})
        self.assertEqual(e["flagged_member_cells"], int(np.nansum(np.where(self._member(), self.got, np.nan))))
        self.assertEqual(spec["source_columns"], e["source_columns"])
        names = {Path(s["path"]).name for s in e["sources"]}
        self.assertTrue({"events.parquet", "filings.parquet", "manifest.json", "links.parquet"} <= names)
        checks = self.manifest["source_checks"]["v9"][NAME]
        ev, fi = checks["events"], checks["filings"]
        self.assertEqual(ev["rows_total"], len(self.w.events))
        self.assertEqual(ev["rows_not_nt_form"], len(NOISE))
        self.assertEqual(ev["rows_nt_amendment"], 1)
        self.assertEqual(ev["rows_sealed"], 1)                  # 2025-03-03 (the window conftest.py binds)
        self.assertEqual(ev["rows_after_role"], 2)              # 2023-08-01 and 2024-02-01
        self.assertEqual(ev["rows_unlinked_cik"], 1)
        self.assertEqual(ev["accessions_with_clock_disagreement"], 1)
        # first: NT 10-K of 1003 (2022), 1004, 1006, 1010, 1011 (2021); NT 10-Q of 1001, 1003 (2021), 1007, 1009,
        # 1011 (2022), 1013; NT 20-F of 1005, 1012
        self.assertEqual(ev["notices_first"], {"NT 10-K": 5, "NT 10-Q": 6, "NT 20-F": 2})
        self.assertEqual(ev["notices_used"], {"NT 10-K": 8, "NT 10-Q": 9, "NT 20-F": 2})
        # periodic rows after the role: 2023-08-09 and 2023-11-08 of the 10 domestic filers, two 2024 10-Ks
        self.assertEqual((fi["rows_sealed"], fi["rows_after_role"], fi["rows_periodic_amendment"]), (1, 22, 1))
        self.assertEqual(fi["rows_used"], 10 * len(DOMESTIC) + 4)

    def _member(self):
        return np.fromfile(self.w.role / "member.u8", dtype="u1").reshape(len(SESSIONS), len(IDS)) != 0

    def test_field_at_t_unchanged_when_rows_after_t_mutate(self):
        """Every stage row with available_at at or after the 22:00 UTC mark of session t = CUT changes (removed,
        re-formed, added): rows 0..t (and t+1: the clock lags one session) are bit-identical; later rows move."""
        cut = mark(SESSIONS[CUT])
        events = [r for r in self.w.events if r[3] < cut]
        events += [(c, a_, "NT 10-Q" if f == "NT 10-K" else f, a, amend, *rest)
                   for c, a_, f, a, amend, *rest in self.w.events if a >= cut and c == 1009]
        events += [(1014, acc(1014, cut, 800), "NT 10-Q", cut, False, "late_filing_nt_10q", None, "form"),
                   (1004, acc(1004, cut, 801), "NT 10-K", cut + dt.timedelta(hours=1), False, "late_filing_nt_10k",
                    None, "form")]
        filings = [r for r in self.w.filings if r[2] < cut] + [(1006, "10-Q", cut + dt.timedelta(hours=2), False)]
        late = World(self.w.base, events=events, filings=filings, name="late")
        late.run("late-nt")
        row = len(IDS) * 8
        before = (self.w.base / "nt" / f"{NAME}.f64").read_bytes()
        after = (self.w.base / "late-nt" / f"{NAME}.f64").read_bytes()
        self.assertEqual(after[:(CUT + 2) * row], before[:(CUT + 2) * row])
        self.assertNotEqual(after[(CUT + 2) * row:], before[(CUT + 2) * row:])
        got = late.field("late-nt")
        self.assertEqual(got[CUT + 2, IDS.index(114)], 1.0)                 # the notice at the mark: usable t+2
        self.assertEqual(got[-1, IDS.index(106)], 0.0)                       # presence extended by the new 10-Q
        np.testing.assert_array_equal(got, oracle(events, filings))

    def test_seal_from_the_research_window(self):
        """A seal inside the role (the builder's SEAL_NS, which research_window sets): stage rows at or after it are
        dropped by the reader. Rows that would move values are added after it: the sealed output equals the world
        without them; unsealed, they move it."""
        seal = mark(SESSIONS[CUT])
        loud = World(self.w.base, name="loud",
                     events=self.w.events + [(1014, acc(1014, seal, 810), "NT 10-Q", seal + dt.timedelta(days=1),
                                              False, "late_filing_nt_10q", None, "form")],
                     filings=self.w.filings + [(1006, "10-Q", seal + dt.timedelta(days=2), False)])
        with mock.patch.object(tool, "SEAL_NS", int(seal.timestamp()) * 10 ** 9):
            m = self.w.run("sealed")
            m_loud = loud.run("sealed-loud")
        self.assertEqual(m_loud["files"][f"{NAME}.f64"], m["files"][f"{NAME}.f64"])
        checks = m["source_checks"]["v9"][NAME]
        dropped = sum(1 for r in self.w.events if r[2] in ("NT 10-K", "NT 10-Q", "NT 20-F") and r[3] >= seal)
        self.assertEqual(checks["events"]["rows_sealed"], dropped)
        self.assertEqual(m_loud["source_checks"]["v9"][NAME]["events"]["rows_sealed"], dropped + 1)
        np.testing.assert_array_equal(self.w.field("sealed"), oracle(self.w.events, self.w.filings, seal=seal))
        unsealed = loud.run("unsealed-loud")
        self.assertNotEqual(unsealed["files"][f"{NAME}.f64"], self.manifest["files"][f"{NAME}.f64"])

    def test_repository_window_drops_every_row_dated_2024_or_later(self):
        """In a fresh interpreter (no conftest.py: the repository window, seal 2024-01-01) the builder with the draft
        registered drops every stage row available on or after 2024-01-01 as sealed; the payload is the one built
        here (where those rows lie after the role)."""
        code = ("import json, sys, contextlib, io\nfrom pathlib import Path\n"
                "import prepare_research_fields as t, prepare_research_fields_draft as d, research_window as rw\n"
                "d.register(vars(t))\na = json.loads(sys.argv[1])\n"
                "with contextlib.redirect_stdout(io.StringIO()):\n"
                "    m = t.run(Path(a['role']), a['role_sha'], Path(a['out']), ['nt_first_126'],\n"
                "              module_options={k: (Path(v) if k in ('sec_stages', 'sec_identity_bridge') else v)\n"
                "                              for k, v in a['options'].items()})\n"
                "c = m['source_checks']['v9']['nt_first_126']\n"
                "print(json.dumps({'seal': rw.SEAL_DATE, 'payload': m['files']['nt_first_126.f64'],\n"
                "                  'events': c['events'], 'filings': c['filings'], 'manifest_seal': m['seal']}))\n")
        opts = {k: str(v) for k, v in self.w.options().items()}
        args = json.dumps({"role": str(self.w.role), "role_sha": self.w.role_sha, "out": str(self.w.base / "repo"),
                           "options": opts})
        got = isolated(TOOLS, code, args)
        self.assertEqual(got["seal"], "2024-01-01")
        self.assertEqual(got["manifest_seal"]["exclusive_end"], "2024-01-01")
        self.assertEqual(got["payload"], self.manifest["files"][f"{NAME}.f64"])
        sealed_nt = sum(1 for r in self.w.events if r[2].startswith("NT") and not r[4] and r[3] >= at(2024, 1, 1))
        self.assertEqual((got["events"]["rows_sealed"], got["events"]["rows_after_role"]), (sealed_nt, 1))
        self.assertEqual(got["filings"]["rows_sealed"], sum(1 for r in self.w.filings if r[2] >= at(2024, 1, 1)))

    def test_byte_identity_with_the_8k_fields(self):
        """The 8-K fields (same stage) are identical with and without the draft field, and it with or without them."""
        alone = self.w.run("k8", fields=K8_RUN)
        both = self.w.run("both", fields=K8_RUN + [NAME])
        for name in K8_RUN:
            self.assertEqual(both["files"][f"{name}.f64"], alone["files"][f"{name}.f64"], name)
            self.assertEqual(entry(both, name), entry(alone, name), name)
        self.assertEqual(both["source_checks"]["sec"], alone["source_checks"]["sec"])
        self.assertEqual(set(both["source_checks"]) - set(alone["source_checks"]), {"v9"})
        self.assertEqual(both["files"][f"{NAME}.f64"], self.manifest["files"][f"{NAME}.f64"])
        self.assertEqual([e["name"] for e in both["fields"]], K8_RUN + [NAME])     # registry order: draft last

    def test_reuse(self):
        again = self.w.run("again", reuse=self.w.base / "nt")
        self.assertEqual((again["reuse"]["reused"], again["reuse"]["computed"]), ([NAME], []))
        e = dict(entry(again))
        self.assertEqual(e.pop("reused_from")["inputs"], v9.entry_inputs(entry(self.manifest)))
        self.assertEqual(e, entry(self.manifest))
        self.assertEqual(again["source_checks"]["v9"], self.manifest["source_checks"]["v9"])
        prior = self.w.base / "k8-prior"
        self.w.run("k8-prior", fields=K8_RUN)
        mixed = self.w.run("mixed", fields=K8_RUN + [NAME], reuse=prior)    # fields v13 from a v12-like prior
        self.assertEqual((mixed["reuse"]["reused"], mixed["reuse"]["computed"]), (K8_RUN, [NAME]))
        self.assertEqual(mixed["files"][f"{NAME}.f64"], self.manifest["files"][f"{NAME}.f64"])
        moved = {**v9.imported_code("v9_nt"), "sha256": "0" * 64}
        with mock.patch.object(v9, "imported_code", lambda group, *a, **k: moved):
            edited = self.w.run("edited", reuse=self.w.base / "nt")
        self.assertIn("inputs differ", edited["reuse"]["not_reused"][NAME])
        pin = self.w.run("pin", reuse=self.w.base / "nt",
                         module_options=self.w.options(sec_filings_sha256=self.w.stage_sha.upper()))
        self.assertEqual(pin["reuse"]["reused"], [NAME])                    # pins compare lower-cased

    def test_refusals_before_output_and_cli(self):
        for bad in ("sec_stages", "sec_identity_bridge_sha256", "sec_filings_sha256"):
            opts = {k: v for k, v in self.w.options().items() if k != bad}
            with self.assertRaisesRegex(ValueError, "--" + bad.replace("_", "-")):
                self.w.run("refused", module_options=opts)
            self.assertFalse((self.w.base / "refused").exists())
        with self.assertRaisesRegex(ValueError, "sec-filings manifest SHA-256"):
            self.w.run("wrongpin", module_options=self.w.options(sec_filings_sha256="0" * 64))
        with tempfile.TemporaryDirectory() as temp:
            w = World(Path(temp))
            path = w.stages / "sec_filings" / "events.parquet"
            path.write_bytes(path.read_bytes() + b"\0")
            with self.assertRaisesRegex(ValueError, "does not match its manifest entry"):
                w.run("tampered")
        o = self.w.options()
        argv = ["--role", str(self.w.role), "--role-sha256", self.w.role_sha, "--output", str(self.w.base / "cli"),
                "--fields", NAME, "--sec-stages", str(o["sec_stages"]), "--sec-identity-bridge",
                str(o["sec_identity_bridge"]), "--sec-identity-bridge-sha256", o["sec_identity_bridge_sha256"],
                "--sec-filings-sha256", o["sec_filings_sha256"]]
        with contextlib.redirect_stdout(io.StringIO()):
            draft.main(argv)
        m = json.loads((self.w.base / "cli" / "manifest.json").read_bytes())
        self.assertEqual(m["files"][f"{NAME}.f64"], self.manifest["files"][f"{NAME}.f64"])
        self.assertEqual(m["code_sha256_lf"], self.manifest["code_sha256_lf"])   # the builder's own identity


class FirstFlags(unittest.TestCase):
    def test_first_flags_match_brute_force(self):
        rng = np.random.default_rng(13)
        n = 400
        cidx = rng.integers(0, 12, n)
        day = 86_400 * 10 ** 9
        avail = rng.integers(0, 1500, n) * day + rng.choice([0, 1, day // 2], n)
        avail[:20] = avail[20:40] + 365 * day                        # boundaries: exactly 365 days apart
        cidx[:20] = cidx[20:40]
        got = v9.first_flags(cidx, avail, 365 * day)
        for i in range(n):
            prior = [j for j in range(n) if cidx[j] == cidx[i] and avail[i] - 365 * day <= avail[j] < avail[i]]
            self.assertEqual(bool(got[i]), not prior, i)
        self.assertGreater(int(got.sum()), 10)
        self.assertGreater(int((~got).sum()), 100)
        self.assertEqual(v9.first_flags(np.zeros(0, np.int64), np.zeros(0, np.int64), day).tolist(), [])


if __name__ == "__main__":
    unittest.main()
