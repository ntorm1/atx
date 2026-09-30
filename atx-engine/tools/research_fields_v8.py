"""Platform v8 research fields (lane F3): ``grp_ff12f49`` and ``gscore7_lowbm`` (specs F-A and F-C of the library v8
draft, section 8; Ruling E-20). F-B (``k8_item402_63``) lives in research_fields_sec.py's 8-K pass.

An opt-in field module of ``prepare_research_fields.py``, like ``research_fields_price.py``: the builder's
``FIELD_MODULES`` hook binds it (``bind(globals())``), ``check`` runs before any output and ``compute`` after the
built-in groups and the modules registered before it. Nothing else in the builder changes, so every other field's
bytes and manifest entry are identical whether or not these fields are requested.

Registration: ``bind`` adds ``FIELDS`` to the builder's ``ALL_FIELDS`` itself (after every field registered before
it), so the builder hook is the import and ``FIELD_MODULES.append(...)`` only. A module-level
``ALL_FIELDS.update(...)`` statement in the builder would join the host closure of every module that reads
``spec_definition`` through its handle (the SEC module), change the SEC producer fingerprint and so recompute every
SEC field once under ``--reuse``.

``--reuse`` (v8 C-3 contract): ``PRODUCERS``, ``HOST_HANDLES``, ``producer_group``, ``field_spec``, ``reuse_inputs``
and ``entry_inputs``; every computed entry records ``producer`` (this module's code identity).

Fields:
* ``grp_ff12f49``: FF12 with the Money industry split by FF49 (Banks, Insur, RlEst, Fin): a cellwise function of this
  run's ``grp_ff12`` and ``grp_ff49`` payloads (same SIC row, clock and 550-day staleness). It lets a DSL member rank
  financial firms within their FF49 industry and every other firm within its FF12 industry (R2-8, S-12), which no DSL
  expression can do (group fields cannot be compared or selected).
* ``gscore7_lowbm`` (R7-4): Mohanram's (2005) G-score without advertising, on the low book-to-market tercile, peers
  of the same two-digit SIC.

The fiscal-quarter fields need many fiscal quarters, more than any daily panel of a 4-year role holds. They read the
issuer fields' own inputs (``--identity-bridge``, ``--fund-events``, their SHA-256 pins and ``--fund-lag-sessions``: the
builder's argument names are this module's ``OPTIONS``, so ``main()`` hands over the same parsed values; ``run()``
callers pass them in ``module_options`` as well) with the builder's loaders, through ``h`` (fingerprinted). Each events
row carries a view of the CIK's fiscal quarters: every row of the CIK up to it in clock order (``QUARTER_RULE``), all
visible whenever that row is the selected one, so a value at session t uses nothing that is not visible at t. The row
of session t is the issuer fields' own (latest visible row, lag, staleness, primary lines). Everything on or after the
research seal is dropped by the builder's loaders (``SEAL_NS``, research_window.py).
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import sys

import numpy as np

from research_fields_sec import _Host  # the SEC module's late-bound host view (same directory)

GROUP = "v8"
# the builder's own arguments of the issuer fields (the fiscal-quarter fields): main() passes their parsed values
OPTIONS = ("identity_bridge", "identity_bridge_sha256", "fund_events", "fund_events_sha256", "fund_lag_sessions")
HOST_HANDLES = ("h",)
FF12_MONEY = 11.0                       # French Siccodes12 "Money" (prepare_research_fields.FF12_NUMBERS)
FF49_MONEY = (45.0, 46.0, 47.0, 48.0)   # French Siccodes49 Banks, Insur, RlEst, Fin
MONEY_OFFSET = 100.0
MONEY_CODES = {"145": "Banks", "146": "Insur", "147": "RlEst", "148": "Fin", "11": "Money, no FF49 industry"}

FF12F49_RULE = ("ff12-money-ff49-v1, cellwise on row t: ff12 = grp_ff12[t], ff49 = grp_ff49[t]; 100 + ff49 when "
                "ff12 == 11 (Money) and ff49 in {45, 46, 47, 48} (145 Banks, 146 Insur, 147 RlEst, 148 Fin); "
                "otherwise ff12 (a Money SIC that Siccodes49 does not list -> 11; every non-Money cell -> ff12 bit for "
                "bit, 1-10 and 12; ff12 NaN -> NaN)")
FF12F49_CLOCK = ("the clock of this run's grp_ff12 and grp_ff49 (fund-events-lagged-v1 on the SIC table: the linked "
                 "CIK's latest visible valid SIC row, --fund-lag-sessions; both from the same row): row t reads their "
                 "row t only")

FIELDS = {
    "grp_ff12f49": {
        "group": "v8_ff12f49", "point_in_time": True, "lagged": False,
        "units": ("categorical code: Fama-French 12-industry number with FF12 Money (11) split by FF49: 145 Banks, "
                  "146 Insur, 147 RlEst, 148 Fin; 11 for a Money SIC listed under no FF49 industry; 1-10 and 12 "
                  "otherwise; stored as f64"),
        "clock": FF12F49_CLOCK,
        "staleness": ("inherits grp_ff12 (no visible SIC row or a row older than 550 days -> NaN): NaN wherever "
                      "grp_ff12 is NaN; no fill"),
        "definition": FF12F49_RULE,
        "source_columns": ["grp_ff12", "grp_ff49"],
        "caveats": [
            "REITs (SIC 6798) are FF49 48 Fin (Trading) in French's Siccodes49, not 47 RlEst",
            "a Money SIC that Siccodes49 does not list keeps the FF12 Money code 11 (one group of such firms)",
            "inherits every grp_ff12 / grp_ff49 caveat: SIC as of each filing (FSDS SUB), the pinned French mapping "
            "tables, identity from the pinned identity bridge, values on primary lines only"],
        "formula_id": "ff12-money-ff49-v1",
        "min_history": "as grp_ff12 (one visible valid SIC row)",
        "requires": ["grp_ff12", "grp_ff49"],
    },
}

# ---- F-C, F-D: fiscal-quarter history of the issuer fields' events rows ---------------------------------------------
QUARTER_NUM, QUARTER_DEN = 913_125, 10_000   # one fiscal quarter = 91.3125 days (365.25 / 4), in integer arithmetic
QUARTER_TOL_DAYS = 20                        # the events contract's tolerance at lag dates
KEY_SPAN = 1 << 22                           # epoch days, row positions and pair indices stay below it (asserted)
QUARTER_CHUNK = 1 << 15                      # events rows per vectorised block
GS_WINDOW, GS_MIN_FINITE = 16, 12
GS_SIGNALS = (("roa", True), ("cfroa", True), ("varroa", False), ("varsgr", False), ("rda", True), ("capxa", True))
QUARTER_ITEMS = {"gscore7_lowbm": ("be", "at", "ni_ttm", "cfo_ttm", "capx_ttm", "xrd_ttm", "ni_q", "sale_ttm")}
QUARTER_NAMES = tuple(QUARTER_ITEMS)

QUARTER_RULE = (
    "fiscal-quarter-view-v1: events row r (anchor A = its period_end) sees the CIK's events rows up to r in "
    "(accepted_utc, accession) order; its fiscal quarter k (k = 0 is A's) is the period_end P_k among them nearest "
    "A - floor(91.3125 k + 0.5) days within 20 days (ties: the later period_end), with the items of the latest row up to "
    "r anchored at P_k; none within 20 days -> the quarter is missing (NaN items); a null period_end is never a quarter")
ISSUER_CLOCK = (
    "fund-events-lagged-v1, the issuer fields' clock and link (--identity-bridge, --fund-events, --fund-lag-sessions L "
    "of the builder): the linked CIK's latest events row with accepted_utc < date(session t-L) 22:00 UTC (max "
    "accepted_utc, tie by accession) on primary (P) lines only; sessions t < L -> NaN; the row's fiscal-quarter view "
    "(QUARTER_RULE) holds only rows up to it, all visible at t")
ISSUER_STALENESS = (
    "the issuer fields' events rule: the selected row is stale, and the value NaN, when date(t) - period_end > "
    "staleness_days (200, or 400 for an annual-only filer); no visible row, a secondary or unlinked line -> NaN; no fill")
QUARTER_CAVEATS = [
    "a fiscal quarter's items are those last reported while it was the anchor (the latest row anchored at it, up to the "
    "selected row); a later restatement that arrives with a later anchor updates only that anchor's lag items, not "
    "this view",
    "events values are modeled/unaccepted (the fundamental events producer over CompanyFacts CF-R); the events emit rows "
    "from 2014-06-01, so the full history is reachable from about 2018",
    "identity from the pinned identity bridge (--identity-bridge), values on primary lines only"]
GSCORE_RULE = (
    "mohanram-g7-lowbm3-sic2-v1 at session t: bm = be / me_company[t-1] (be of the selected events row > 0, this run's "
    "me_company of session t-1 > 0); U = member names at t (member.u8) with a finite bm; bottom tercile T = the names in U "
    "with bm <= the 1/3 quantile of bm over U (numpy linear interpolation); outside T -> NaN. Inputs of the selected row "
    "(at > 0): ROA = ni_ttm / at, CFROA = cfo_ttm / at, RDA = xrd_ttm / at (a missing xrd_ttm -> 0), CAPXA = capx_ttm / "
    "at; VARROA = sample variance (ddof 1) of ni_q / at over fiscal quarters k = 0..15, VARSGR = sample variance of "
    "sale_ttm(P_k) / sale_ttm(P_(k+1)) - 1 (sale_ttm(P_(k+1)) > 0) over k = 0..15 (QUARTER_RULE), each with at least "
    "12 finite quarters. A name in T is scored when grp_sic2[t] and every input but R&D is finite, else NaN. Peer "
    "median of a measure: np.median of that measure over the names in T of the same grp_sic2 at t for which it is "
    "finite. Value = [ROA > median] + [CFROA > median] + [cfo_ttm > ni_ttm] + [VARROA < median] + [VARSGR < median] + "
    "[RDA > median] + [CAPXA > median]")
_EVENT_COLS = ["cik", "accepted_utc", "accession", "period_end", "staleness_days"]
FIELDS["gscore7_lowbm"] = {
    "group": "v8_gscore", "point_in_time": True, "lagged": False,
    "units": ("count 0-7: Mohanram (2005) G-score signals met (advertising omitted) by a low book-to-market name; "
              "stored as f64"),
    "clock": ISSUER_CLOCK + "; me_company of session t-1 and grp_sic2 of session t: this run's payloads",
    "staleness": ISSUER_STALENESS + "; outside the bottom bm tercile or a missing input other than R&D -> NaN",
    "definition": GSCORE_RULE + ". " + QUARTER_RULE,
    "source_columns": list(QUARTER_ITEMS["gscore7_lowbm"]) + _EVENT_COLS + ["me_company", "grp_sic2", "member.u8"],
    "caveats": [
        "declared deviations from Mohanram (2005): the bottom bm tercile (not quintile), ratios scaled by at of the "
        "latest period end (not beginning assets), no advertising signal (seven signals), peers = member names of the "
        "same grp_sic2 (Ruling E-20)",
        "a sic2 group with a single scored name compares it with itself: every median signal is 0",
        *QUARTER_CAVEATS],
    "formula_id": "mohanram-g7-lowbm3-sic2-v1",
    "min_history": "17 fiscal quarters of events rows (VARSGR), at least 12 finite in each variance",
    "requires": ["me_company", "grp_sic2"],
}
# The producing code of each field group (contract of task C-3: {group: (entry names,)}); the module-level
# definitions they reach are part of each producer. compute() orchestrates and is never a producer.
PRODUCERS = {"v8_ff12f49": ("ff12f49_rows",),
             "v8_gscore": ("issuer_history", "gscore_rows")}


# ---------------------------------------------------------------------------------------------------------------
# Inputs: this run's published payloads
# ---------------------------------------------------------------------------------------------------------------

class PublishedRows:
    """This run's published payloads of ``names`` (fields a module field requires), read row by row in session order
    and hashed as read. Each must be a complete date-major f64 file of the role's shape."""

    def __init__(self, output: Path, role, names, what: str):
        self.what, self.size, self.n_dates = what, role.n * 8, role.n_dates
        self.paths = {x: Path(output) / f"{x}.f64" for x in names}
        for x, p in self.paths.items():
            if not p.is_file() or p.stat().st_size != role.n_dates * self.size:
                raise ValueError(f"{what}: this run's {x}.f64 is missing or has the wrong size")
        self.hashes = {x: hashlib.sha256() for x in names}
        self.files = {}
        try:
            for x, p in self.paths.items():
                self.files[x] = p.open("rb")
        except BaseException:
            self.close()
            raise

    def row(self) -> dict:
        out = {}
        for x, f in self.files.items():
            blob = f.read(self.size)
            if len(blob) != self.size:
                raise ValueError(f"{self.what}: this run's {x}.f64 is truncated")
            self.hashes[x].update(blob)
            out[x] = np.frombuffer(blob, dtype="<f8")
        return out

    def sources(self) -> list:
        """One source per payload read (after every row): path, bytes, SHA-256 of the bytes read, field."""
        for x, f in self.files.items():
            if f.read(1):
                raise ValueError(f"{self.what}: this run's {x}.f64 is longer than the role shape")
        return [{"path": str(p.resolve()), "bytes": self.n_dates * self.size, "sha256": self.hashes[x].hexdigest(),
                 "field": x} for x, p in self.paths.items()]

    def close(self):
        for f in self.files.values():
            f.close()


def issuer_history(h, role, budget, options: dict, items) -> dict:
    """The issuer fields' inputs, read with the builder's own loaders: the link matrix of the pinned identity bridge
    and the linked CIKs' events rows (clock order, sealed rows dropped) with ``items``; their sources and checks."""
    budget.admit(role.n_dates * role.n * 5 + (64 << 20), "v8-issuer-link-matrix")
    links, bridge_src, bridge_st = h.load_bridge(Path(options["identity_bridge"]), options["identity_bridge_sha256"],
                                                 role, budget)
    marks = role.days * h.DAY_NS + h.MARK_NS
    ciks, link, primary, link_st = h.resolve_links(links, role, marks, budget)
    del links
    events = Path(options["fund_events"])
    m, man_src = h.pinned_manifest(events, options["fund_events_sha256"], "fund-events", h.EVENTS_ADAPTER["schema"])
    ev, ev_src, ev_st = h.load_events(events, m, h.EVENTS_ADAPTER, list(items), ciks, "fund-events", budget)
    ev["known"] = ev["period_end"] != h.NULL_PERIOD_DAY
    return {"ciks": ciks, "link": link, "primary": primary, "marks": marks, "ev": ev,
            "sources": list(bridge_src) + [man_src] + ev_src,
            "pins": {"identity_bridge_manifest_sha256": bridge_src[0]["sha256"],
                     "fund_events_manifest_sha256": man_src["sha256"]},
            "checks": {"identity_bridge": {**bridge_st, **link_st}, "fund_events": ev_st}}


class LatestRows:
    """Session by session, the issuer fields' selection: each line's latest events row with clock < the mark of
    session t-L (primary lines only), and whether it is usable (visible and not stale by the events rule)."""

    def __init__(self, h, hist: dict, lag: int):
        self.h, self.hist, self.lag, self.p = h, hist, lag, 0
        self.latest = np.full(len(hist["ciks"]), -1, dtype=np.int64)

    def at(self, t: int, day: int):
        """(row index per line, clipped to >= 0; usable mask; primary-link mask) at session t (called in order)."""
        ev, hist = self.hist["ev"], self.hist
        if t >= self.lag:
            self.p = self.h.advance(ev, self.latest, self.p, int(hist["marks"][t - self.lag]))
        lk = hist["link"][t]
        pline = (lk >= 0) & hist["primary"][t]
        e = np.where(pline, self.latest[np.maximum(lk, 0)], -1) if len(self.latest) else np.full(len(lk), -1)
        es = np.maximum(e, 0)
        use = np.zeros(len(lk), dtype=bool)
        if len(ev["clock"]):
            use = (e >= 0) & ((day - ev["period_end"][es]) <= ev["stale_days"][es])
        return es, use, pline


class QuarterIndex:
    """``QUARTER_RULE`` over the clock-ordered events rows of load_events (``cidx``, ``period_end``, ``known``)."""

    def __init__(self, ev: dict):
        cidx, anchor, known = ev["cidx"], ev["period_end"], ev["known"]
        n = len(cidx)
        if n >= KEY_SPAN or (known.any() and (anchor[known].min() < 0 or anchor[known].max() >= KEY_SPAN)):
            raise ValueError("v8 fiscal quarters: events rows or period_end days outside the key range")
        self.cidx, self.anchor, self.known = cidx, anchor, known
        pos = np.flatnonzero(known)
        o = np.lexsort((pos, anchor[pos], cidx[pos]))                 # (cik, period_end, clock position)
        sc, sa, self.sp = cidx[pos][o], anchor[pos][o], pos[o]
        new = np.ones(len(o), dtype=bool)
        new[1:] = (sc[1:] != sc[:-1]) | (sa[1:] != sa[:-1])
        self.pair_key = sc[new] * KEY_SPAN + sa[new]                  # distinct (cik, period_end), ascending
        self.pair_anchor, self.pair_first = sa[new], self.sp[new]     # ... and the first row that carries it
        self.row_key = (np.cumsum(new) - 1) * KEY_SPAN + self.sp      # (pair, position), ascending

    def rows(self, r: np.ndarray, quarters: int) -> np.ndarray:
        """(len(r), quarters) row indices: fiscal quarter k of each row r as known at r; -1 when missing."""
        off = (QUARTER_NUM * np.arange(quarters, dtype=np.int64) + QUARTER_DEN // 2) // QUARTER_DEN
        target = self.anchor[r][:, None] - off[None, :]
        base = self.cidx[r][:, None] * KEY_SPAN
        lo = np.searchsorted(self.pair_key, base + np.clip(target - QUARTER_TOL_DAYS, 0, KEY_SPAN - 1), side="left")
        hi = np.searchsorted(self.pair_key, base + np.clip(target + QUARTER_TOL_DAYS, 0, KEY_SPAN - 1), side="right")
        best = np.full(target.shape, -1, dtype=np.int64)
        best_d = np.full(target.shape, QUARTER_TOL_DAYS, dtype=np.int64)   # d <= best_d: within the tolerance
        rr = r[:, None]
        for c in range(int((hi - lo).max()) if hi.size else 0):   # candidates ascend by period_end
            g = np.minimum(lo + c, max(len(self.pair_key) - 1, 0))
            d = np.abs(self.pair_anchor[g] - target)
            take = (lo + c < hi) & (self.pair_first[g] <= rr) & (d <= best_d)   # ties: the later period_end
            best, best_d = np.where(take, g, best), np.where(take, d, best_d)
        at = np.searchsorted(self.row_key, np.maximum(best, 0) * KEY_SPAN + rr, side="right") - 1
        out = np.where(best >= 0, self.sp[np.maximum(at, 0)] if len(self.sp) else -1, -1)
        return np.where(self.known[r][:, None], out, -1)


def gathered(values: np.ndarray, rows: np.ndarray) -> np.ndarray:
    """values[rows] with NaN where rows is -1."""
    return np.where(rows >= 0, values[np.maximum(rows, 0)], np.nan) if len(values) else np.full(rows.shape, np.nan)


def picked(values: np.ndarray, e: np.ndarray, use: np.ndarray) -> np.ndarray:
    """A per-events-row value at the selected rows ``e`` where usable, else NaN."""
    return np.where(use, values[e], np.nan) if len(values) else np.full(len(e), np.nan)


def sample_variance(x: np.ndarray, min_finite: int) -> np.ndarray:
    """Per row: the sample variance (ddof 1) of the finite values; NaN with fewer than ``min_finite``."""
    fin = np.isfinite(x)
    c = fin.sum(axis=1)
    with np.errstate(invalid="ignore", divide="ignore"):
        mean = np.where(fin, x, 0.0).sum(axis=1) / c
        dev = np.where(fin, x - mean[:, None], 0.0)
        var = (dev * dev).sum(axis=1) / (c - 1)
    return np.where(c >= min_finite, var, np.nan)


# ---------------------------------------------------------------------------------------------------------------
# Producers: each returns {name: (writer, sources, extras)}
# ---------------------------------------------------------------------------------------------------------------

def ff12f49_code(ff12: np.ndarray, ff49: np.ndarray) -> np.ndarray:
    """``FF12F49_RULE`` on arrays of FF12 and FF49 codes (NaN = unknown label)."""
    money = (ff12 == FF12_MONEY) & np.isin(ff49, FF49_MONEY)
    return np.where(money, MONEY_OFFSET + ff49, ff12)


def ff12f49_rows(h, role, output: Path, budget) -> dict:
    """``grp_ff12f49`` (its field definition) from this run's published grp_ff12.f64 and grp_ff49.f64."""
    rows = PublishedRows(output, role, ("grp_ff12", "grp_ff49"), "grp_ff12f49")
    counts = dict.fromkeys(MONEY_CODES, 0)
    w = None
    try:
        try:
            w = h.FieldWriter(output, "grp_ff12f49", role)
            for t in range(role.n_dates):
                got = rows.row()
                code = ff12f49_code(got["grp_ff12"], got["grp_ff49"])
                w.write(code)
                member = role.member[t] != 0
                for key in counts:
                    counts[key] += int(np.count_nonzero(member & (code == float(key))))
                if t % 256 == 0:
                    budget.check("v8-ff12f49-write")
            sources = rows.sources()
        finally:
            rows.close()
    except BaseException:
        if w is not None:
            w.f.close()   # refused: the partial file stays unpublished (no manifest), handle released
        raise
    w.close()
    return {"grp_ff12f49": (w, sources, {"money_member_cells": counts})}


def gscore_history(ev: dict, index: QuarterIndex) -> tuple:
    """VARROA and VARSGR (``GSCORE_RULE``) of every events row."""
    n, v = len(ev["clock"]), ev["values"]
    var_roa, var_sg = np.full(n, np.nan), np.full(n, np.nan)
    for lo in range(0, n, QUARTER_CHUNK):
        r = np.arange(lo, min(lo + QUARTER_CHUNK, n))
        q = index.rows(r, GS_WINDOW + 1)
        with np.errstate(invalid="ignore", divide="ignore"):
            at = gathered(v["at"], q[:, :GS_WINDOW])
            roa = np.where(at > 0, gathered(v["ni_q"], q[:, :GS_WINDOW]) / at, np.nan)
            sale = gathered(v["sale_ttm"], q)
            sg = np.where(sale[:, 1:] > 0, sale[:, :GS_WINDOW] / sale[:, 1:] - 1.0, np.nan)
        var_roa[r], var_sg[r] = sample_variance(roa, GS_MIN_FINITE), sample_variance(sg, GS_MIN_FINITE)
    return var_roa, var_sg


def group_median(codes: np.ndarray, values: np.ndarray) -> np.ndarray:
    """Per element: np.median of ``values`` over the elements with the same code (values finite)."""
    _, inv, counts = np.unique(codes, return_inverse=True, return_counts=True)
    o = np.lexsort((values, inv))
    v = values[o]
    start = np.concatenate(([0], np.cumsum(counts)[:-1]))
    return ((v[start + (counts - 1) // 2] + v[start + counts // 2]) / 2)[inv]


def gscore_cross_section(member: np.ndarray, bm: np.ndarray, sic2: np.ndarray, x: dict) -> tuple:
    """``GSCORE_RULE`` at one session: (value, tercile mask, scored mask). ``x`` holds each GS_SIGNALS measure (NaN =
    missing) and ``cfo_gt_ni`` (bool). A measure's peer median is over the tercile names of the same sic2 with that
    measure finite (a name missing another input still informs it, but is not scored)."""
    value = np.full(len(bm), np.nan)
    universe = member & np.isfinite(bm)
    tercile = np.zeros(len(bm), dtype=bool)
    if universe.any():
        tercile = universe & (bm <= np.quantile(bm[universe], 1.0 / 3.0))
    grouped = tercile & np.isfinite(sic2)
    scored = grouped.copy()
    for key, _ in GS_SIGNALS:
        scored &= np.isfinite(x[key])
    s = np.flatnonzero(scored)
    if len(s):
        count = x["cfo_gt_ni"][s].astype(np.float64)
        med = np.full(len(bm), np.nan)
        for key, above in GS_SIGNALS:
            peers = np.flatnonzero(grouped & np.isfinite(x[key]))   # every scored name is a peer of each measure
            med[peers] = group_median(sic2[peers], x[key][peers])
            count += (x[key][s] > med[s]) if above else (x[key][s] < med[s])
        value[s] = count
    return value, tercile, scored


def gscore_rows(h, hist: dict, role, output: Path, budget, lag: int) -> dict:
    """``gscore7_lowbm`` (its field definition) from the issuer history and this run's me_company / grp_sic2."""
    ev = hist["ev"]
    var_roa, var_sg = gscore_history(ev, QuarterIndex(ev))
    budget.check("v8-gscore-history")
    sel = LatestRows(h, hist, lag)
    counts = {"bm_universe": 0, "tercile": 0, "scored": 0}
    rows = PublishedRows(output, role, ("me_company", "grp_sic2"), "gscore7_lowbm")
    w = None
    try:
        try:
            w = h.FieldWriter(output, "gscore7_lowbm", role)
            me_prev = np.full(role.n, np.nan)          # row t reads me_company of session t-1 only
            for t in range(role.n_dates):
                got = rows.row()
                e, use, _ = sel.at(t, int(role.days[t]))
                item = {k: picked(ev["values"][k], e, use) for k in QUARTER_ITEMS["gscore7_lowbm"]}
                at, ni, cfo = item["at"], item["ni_ttm"], item["cfo_ttm"]
                with np.errstate(invalid="ignore", divide="ignore"):
                    a = np.where(at > 0, at, np.nan)
                    x = {"roa": ni / a, "cfroa": cfo / a, "capxa": item["capx_ttm"] / a,
                         "rda": np.where(np.isfinite(item["xrd_ttm"]), item["xrd_ttm"], 0.0) / a,
                         "varroa": picked(var_roa, e, use), "varsgr": picked(var_sg, e, use), "cfo_gt_ni": cfo > ni}
                    bm = np.where((item["be"] > 0) & (me_prev > 0), item["be"] / me_prev, np.nan)
                member = role.member[t] != 0
                value, tercile, scored = gscore_cross_section(member, bm, got["grp_sic2"], x)
                w.write(value)
                me_prev = got["me_company"]
                counts["bm_universe"] += int(np.count_nonzero(member & np.isfinite(bm)))
                counts["tercile"] += int(np.count_nonzero(tercile))
                counts["scored"] += int(np.count_nonzero(scored))
                if t % 256 == 0:
                    budget.check("v8-gscore-write")
            sources = rows.sources()
        finally:
            rows.close()
    except BaseException:
        if w is not None:
            w.f.close()   # refused: the partial file stays unpublished (no manifest), handle released
        raise
    w.close()
    return {"gscore7_lowbm": (w, list(hist["sources"]) + sources, {"member_cells": counts})}


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "V8FieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_v8: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return V8FieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """This run's input pins of a field beyond the fields it requires: none for grp_ff12f49 (the reuse rule's 'every
    field it requires is reused' is its input check); the identity bridge and events manifests and the declared lag
    for the fiscal-quarter fields."""
    if name not in QUARTER_NAMES:
        return {}
    return {"identity_bridge": str(options.get("identity_bridge_sha256") or "").lower(),
            "fund_events": str(options.get("fund_events_sha256") or "").lower(),
            "fund_lag_sessions": options.get("fund_lag_sessions")}


def entry_inputs(entry: dict) -> dict:
    """The same pins as a manifest entry records them."""
    if entry.get("name") not in QUARTER_NAMES:
        return {}
    return {"identity_bridge": entry.get("identity_bridge_manifest_sha256"),
            "fund_events": entry.get("fund_events_manifest_sha256"),
            "fund_lag_sessions": entry.get("fund_lag_sessions")}


class V8FieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        """No new options: grp_ff12f49 reads this run's grp_ff12 / grp_ff49; the fiscal-quarter fields read the
        builder's own --identity-bridge, --fund-events, their pins and --fund-lag-sessions (OPTIONS)."""

    @staticmethod
    def check(selected, options: dict):
        """Before any output: the fiscal-quarter fields need the issuer fields' inputs (the builder checks
        'requires')."""
        names = [f for f in selected if f in QUARTER_NAMES]
        if not names:
            return
        missing = ["--" + k.replace("_", "-") for k in OPTIONS if options.get(k) in (None, "")]
        if missing:
            raise ValueError(f"--fields: {', '.join(names)} need {', '.join(missing)} (the issuer fields' inputs; "
                             "run() callers pass them in module_options too)")
        lag = options["fund_lag_sessions"]
        if isinstance(lag, bool) or not isinstance(lag, int) or not 0 <= lag <= 5:
            raise ValueError("--fund-lag-sessions must be an integer in [0, 5] (declared: 1)")

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        results, pins = {}, {}
        if "grp_ff12f49" in names:
            results.update(ff12f49_rows(h, role, output, budget))
        quarterly = [x for x in names if x in QUARTER_NAMES]
        if quarterly:
            lag = options["fund_lag_sessions"]
            hist = issuer_history(h, role, budget, options, sorted({i for x in quarterly for i in QUARTER_ITEMS[x]}))
            if "gscore7_lowbm" in names:
                results.update(gscore_rows(h, hist, role, output, budget, lag))
            pins = {**hist["pins"], "fund_lag_sessions": lag}
            source_checks[GROUP] = {"fund_lag_sessions": lag, "clock": ISSUER_CLOCK, "quarter_rule": QUARTER_RULE,
                                    **hist["checks"]}
            del hist
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        for x in names:
            w, sources, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, 0)),
                               "min_history": spec["min_history"], **(pins if x in QUARTER_NAMES else {}), **extra}
            outcome[x] = (w, sources, w.coverage())
        budget.report("v8-complete", fields=len(names))
