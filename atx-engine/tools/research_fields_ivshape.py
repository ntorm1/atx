"""Option-surface shape research fields (platform v8 lane YDATA, draft): two TickerHistory3 families no v8 field reads.

Draft and off by default. ``prepare_research_fields.py`` does not register this module (no v8 field list, formula or
producer fingerprint moves); ``prepare_research_fields_ydata.py`` registers it. No new CLI option: the fields read the
role's own vendor source through research_fields_price.py's ``--price-source`` (it must hash to the role manifest's
``source_sha256``).

Fields (``LAG_SESSIONS`` = 1: row t reads vendor rows dated sessions t-21..t-1 only):
* ``iv_skew_21``: the smile slope of Xing, Zhang and Zhao (2010, JFQA) and Yan (2011, JFE). The vendor column ``shD1``
  is "Interpolated 21 day atm vol slope" (SpiderRock TickerHistory3 dictionary): the slope of the fitted volatility curve
  at the money in standardized moneyness ln(K/F) / (sigma_ATM sqrt(T)) (SpiderRock live surfaces: the volatility
  difference between the curve points at moneyness -0.5 and +0.5), interpolated to 21 trading days (30 calendar days,
  the papers' horizon). The dictionary does not state the sign of the difference, so the field orients it by the market
  line: o_t = the sign of the median of SPY's slope over the same 21 sessions (an index smile is put-rich), and the value
  is o_t x the line's slope of session t-1. A positive value is a smile steeper toward the put side than the index's
  orientation; the literature's high-skew names (expensive out-of-the-money puts) score high.
* ``iv_vov_21``: vol-of-vol of Baltussen, van Bekkum and van der Grient (2018, JFQA) "Unknown unknowns": the standard
  deviation (ddof 1) of the line's daily 30-day ATM implied volatility over sessions t-21..t-1 divided by its mean (the
  month's dispersion of expected volatility, scaled by its level). The vendor's ``atmCenI_21d`` is earnings-censored
  (the implied earnings move removed), so a scheduled announcement inside the month does not by itself raise it.

Stamping: the vendor delivers implied-volatility columns at 22:00 America/Chicago (03:00-04:00 UTC of the next day;
atx-db TIER1_V3_STATUS "IV clock"), so the slope and IV of session s are known before the 22:00 UTC mark of s+1. Row t
reads sessions t-21..t-1 only, so every value is known before the 23:00 UTC decision of row t.

Seal (reader side, research_window): ``compute`` refuses unless the builder's ``SEAL`` is ``research_window.SEAL``;
vendor rows dated on or after the seal are skipped and counted; the role ends before the seal (asserted by the builder).

``--reuse`` (v8 C-3): ``PRODUCERS``, ``HOST_HANDLES``, ``producer_group``, ``field_spec``, ``reuse_inputs``,
``entry_inputs``. The inputs are the role (bound by the prior's role binding) and the vendor file, which must hash to the
role's ``source_sha256``; the producers import nothing from another field module, so no further pin is needed.
"""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

import research_window as rw  # same directory: the seal
from research_fields_sec import _Host  # the builder-namespace view every field module uses (same directory)

GROUP = "ivshape"
OPTIONS = ("price_source",)          # research_fields_price.py's --price-source: no new CLI option
HOST_HANDLES = ("h",)                # v8 C-3: the producers read the builder through ``h``
LAG_SESSIONS = 1                     # row t reads rows dated on or before session t - LAG_SESSIONS (the IV clock)
SKEW_COLUMN = "shD1"                 # "Interpolated 21 day atm vol slope" (SpiderRock TickerHistory3 dictionary)
IV_COLUMN = "atmCenI_21d"            # 21 trading days = 30 calendar days: the papers' horizon
TICKER_COLUMN = "ticker_tk"
ORIENT_TICKER = "SPY"                # the market line whose put-rich smile orients the vendor slope
WINDOW = 21                          # one month of sessions (XZZ / Yan monthly horizon; BvBvdG "the past month")
MIN_DAYS = 17                        # declared: about 80% of the window, the house ratio (F-1's 48 of 60)
SLOPE_ABS_MAX = 5.0                  # a slope across one moneyness unit cannot exceed the IV domain's upper bound
FLOAT_TYPES = (pa.float32(), pa.float64())

IV_CLOCK = (
    "ivshape-lag1-v1: row t reads the vendor TickerHistory3 rows of sessions t-21..t-1 only (shD1 and atmCenI_21d of "
    "session s are delivered at 22:00 America/Chicago, by 04:00 UTC of s+1, before the 22:00 UTC mark of s+1); "
    "nothing dated session t or later; rows on or after the seal are never read")
CELL_RULE = (
    "a cell (session s, line) is valid iff exactly one vendor row has that (tradingDate, securityID) key (duplicates "
    "quarantined) and its atmCenI_21d v satisfies float32(0.02) <= v <= float32(5.0) (IV_DOMAIN: a surface exists)")
SKEW_RULE = (
    "xzz-yan-skew-shd1-spy-oriented-v1: S = the vendor shD1 of a valid cell, kept iff finite and 0 < |S| <= 5.0 "
    "(an exact 0.0 is the vendor's empty print); o_t = sign(median of SPY's kept S over sessions t-21..t-1) when at "
    "least 17 are kept and the median is not 0, the market line = the unique vendor securityID whose ticker_tk is "
    "'SPY' on every row read (another securityID -> refused); value at t = o_t x S of the line at session t-1; NaN "
    f"when o_t or S is undefined ({CELL_RULE})")
VOV_RULE = (
    "bvbvdg-vov-atm21-cv-v1: x_s = atmCenI_21d of the line's valid cells s in t-21..t-1 (K of them); value = "
    "sd(x, ddof 1) / mean(x) when K >= 17, else NaN (" + CELL_RULE + ")")
CAVEATS = ["the vendor's fitted surface (SpiderRock), not a strike-level option price; no vintage proof of the "
           "history (historical_vintage_verified false)",
           "one price line's values (the vendor securityID), not an issuer total"]

FIELDS: dict = {}


def _spec(name, group, units, definition, staleness, source_columns, caveats, formula, domain=None):
    spec = {"group": group, "point_in_time": True, "lagged": False, "units": units, "clock": IV_CLOCK,
            "staleness": staleness, "caveats": CAVEATS + caveats, "definition": definition,
            "source_columns": source_columns, "formula_id": formula,
            "min_history": f"{WINDOW} sessions of the role axis (rows 0..{WINDOW - 1} read fewer)",
            "needs": "vendor_iv_surface"}
    if domain is not None:
        spec["domain"] = domain
    FIELDS[name] = spec


_spec("iv_skew_21", "y_skew",
      "vendor ATM volatility slope per unit of standardized moneyness at 21 trading days, signed so that the index's "
      "put-rich smile is positive",
      SKEW_RULE,
      "no fill: the line's session t-1 cell only; an absent, duplicated, empty or out-of-domain cell, or an undefined "
      "SPY orientation -> NaN",
      ["tradingDate", "securityID", TICKER_COLUMN, SKEW_COLUMN, IV_COLUMN],
      ["the slope's vendor sign convention is not documented; it is oriented by SPY's median slope of the same "
       "sessions, so a vendor convention change moves the orientation with it",
       "XZZ measure skew as OTM put IV minus ATM call IV (OptionMetrics); Yan (2011) as the 30-day delta -0.2 put IV "
       "minus the delta 0.5 call IV: the vendor slope is the same shape statistic on a fitted curve, not their exact "
       "strikes"],
      "xzz-yan-skew-shd1-spy-oriented-v1", domain=(-SLOPE_ABS_MAX, SLOPE_ABS_MAX))
_spec("iv_vov_21", "y_vov",
      "coefficient of variation of the daily 30-day ATM implied volatility over the past 21 sessions (decimal)",
      VOV_RULE,
      f"no fill: fewer than {MIN_DAYS} valid cells in t-21..t-1 -> NaN",
      ["tradingDate", "securityID", IV_COLUMN],
      ["Baltussen-van Bekkum-van der Grient's construction is recalled (scaled monthly dispersion of ATM IV), not "
       "re-read from the paper: root verifies before registration",
       "atmCenI_21d is earnings-censored: the paper's IV is not; the censoring removes the announcement run-up"],
      "bvbvdg-vov-atm21-cv-v1")

# The producing code of each field group (contract of task C-3: {group: (entry functions,)}); compute() orchestrates.
PRODUCERS = {"y_skew": ("verify_iv_source", "iv_panel", "skew_rows"),
             "y_vov": ("verify_iv_source", "iv_panel", "vov_rows")}


# ---------------------------------------------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------------------------------------------

def required_columns(names) -> list:
    cols = [IV_COLUMN]
    if "iv_skew_21" in names:
        cols += [SKEW_COLUMN, TICKER_COLUMN]
    return cols


def require_columns(path: Path, names) -> None:
    """Refuse (a footer read, before any output) a price source without the requested fields' columns."""
    schema = pq.ParquetFile(Path(path), memory_map=False).schema_arrow
    for c in required_columns(names):
        i = schema.get_field_index(c)
        ok = i >= 0 and (schema.field(i).type == pa.string() if c == TICKER_COLUMN else
                         schema.field(i).type in FLOAT_TYPES)
        if not ok:
            raise ValueError(f"{', '.join(n for n in names if n in FIELDS)}: --price-source column {c} is missing or "
                             f"of the wrong type ({Path(path)})")


def verify_iv_source(h, path: Path, role, budget):
    """Hash the price source once: it must be the vendor file the role was projected from (the role manifest's
    source_sha256). Returns its file identity, re-checked after every read."""
    path = Path(path)
    captured = h.identity(path)
    budget.report("ivshape-source-hash-start", bytes=captured[2])
    digest = h.sha_file(path, budget)
    if role.source_sha256 is None or digest != role.source_sha256:
        raise ValueError("--price-source SHA-256 differs from the role's source_sha256 (not the file the role was "
                         "projected from)")
    if h.identity(path) != captured:
        raise ValueError("--price-source changed during hashing")
    return captured


def _valid_iv(h, iv: np.ndarray) -> np.ndarray:
    lo, hi = h.IV_DOMAIN
    with np.errstate(invalid="ignore"):
        return np.isfinite(iv) & (iv >= np.float32(lo)) & (iv <= np.float32(hi))


def iv_panel(h, path: Path, captured, role, budget, names) -> dict:
    """The vendor slope and ATM IV of every (role session, role line) key (f32 as stored; duplicates NaN), SPY's per
    role session, and the read statistics. Rows off the role's sessions or dated on or after the seal are not used."""
    path = Path(path)
    pf = pq.ParquetFile(path, memory_map=False)
    require_columns(path, names)
    want_skew = "iv_skew_21" in names
    seal_day = h.day_of(h.SEAL)
    days = role.days.astype(np.int64)
    first, last = int(days[0]), min(int(days[-1]), seal_day - 1)
    nd, n = role.n_dates, role.n
    budget.admit(nd * n * (2 * 4 + 2) + (32 << 20), "ivshape-matrices")
    iv = np.full((nd, n), np.nan, dtype=np.float32)
    sk = np.full((nd, n), np.nan, dtype=np.float32) if want_skew else None
    counts = np.zeros((nd, n), dtype=np.uint16)
    spy = {"d": [], "sid": [], "s": [], "iv": []}
    st = {"rows_scanned": 0, "rows_on_or_after_seal_skipped": 0, "rows_off_calendar": 0, "rows_selected": 0}
    columns = ["tradingDate", "securityID"] + required_columns(names)
    for batch in pf.iter_batches(batch_size=65536, columns=columns, use_threads=False):
        budget.check("ivshape-source-batch")
        st["rows_scanned"] += batch.num_rows
        d = pc.fill_null(batch.column("tradingDate").cast(pa.int32()), -1).to_numpy().astype(np.int64)
        st["rows_on_or_after_seal_skipped"] += int(np.count_nonzero(d >= seal_day))
        inside = (d >= first) & (d <= last)
        sid_all = pc.fill_null(batch.column("securityID"), 0).to_numpy()
        if want_skew:
            hit = pc.fill_null(pc.equal(batch.column(TICKER_COLUMN), ORIENT_TICKER), False).to_numpy(
                zero_copy_only=False) & inside & (sid_all > 0)
            if hit.any():
                k = np.flatnonzero(hit)
                sub = batch.take(pa.array(k, type=pa.int64()))
                spy["d"].append(d[k])
                spy["sid"].append(sid_all[k].astype(np.int64))
                spy["s"].append(sub.column(SKEW_COLUMN).to_numpy(zero_copy_only=False).astype(np.float32))
                spy["iv"].append(sub.column(IV_COLUMN).to_numpy(zero_copy_only=False).astype(np.float32))
        idx = np.flatnonzero(inside)
        if not len(idx):
            continue
        pos, on = role.columns_of(sid_all[idx])
        idx, j, dd = idx[on], pos[on], d[idx][on]
        t = np.minimum(np.searchsorted(days, dd), nd - 1)
        cal = days[t] == dd
        st["rows_off_calendar"] += int(np.count_nonzero(~cal))
        idx, j, t = idx[cal], j[cal], t[cal]
        if not len(idx):
            continue
        st["rows_selected"] += len(idx)
        np.add.at(counts, (t, j), 1)
        sub = batch.take(pa.array(idx, type=pa.int64()))
        iv[t, j] = sub.column(IV_COLUMN).to_numpy(zero_copy_only=False).astype(np.float32)
        if want_skew:
            sk[t, j] = sub.column(SKEW_COLUMN).to_numpy(zero_copy_only=False).astype(np.float32)
    if h.identity(path) != captured:
        raise ValueError("--price-source changed while the surface columns were read")
    dup = counts > 1
    iv[dup] = np.nan
    if want_skew:
        sk[dup] = np.nan
    st["duplicate_keys_quarantined"] = int(np.count_nonzero(dup))
    st["cell_rule"] = CELL_RULE
    out = {"iv": iv, "skew": sk, "stats": st}
    if want_skew:
        out["spy"] = spy_line(h, spy, days)
    return out


def spy_line(h, spy: dict, days: np.ndarray) -> dict:
    """SPY's kept slope per role session (``SKEW_RULE``); refuses when the ticker maps to more than one securityID."""
    cat = {k: np.concatenate(v) if v else np.zeros(0) for k, v in spy.items()}
    ids = np.unique(cat["sid"].astype(np.int64))
    if len(ids) != 1:
        raise ValueError(f"iv_skew_21: ticker {ORIENT_TICKER} maps to {len(ids)} vendor securityIDs "
                         f"({', '.join(str(int(x)) for x in ids[:5])}) inside the role; the market line must be unique")
    nd = len(days)
    e = np.minimum(np.searchsorted(days, cat["d"].astype(np.int64)), nd - 1)
    on = days[e] == cat["d"]
    e = e[on]
    copies = np.bincount(e, minlength=nd)
    s = np.full(nd, np.nan, dtype=np.float32)
    v = np.full(nd, np.nan, dtype=np.float32)
    s[e], v[e] = cat["s"][on], cat["iv"][on]
    dup = copies > 1
    s[dup], v[dup] = np.nan, np.nan
    kept = kept_slope(h, s, v)
    return {"s": kept, "stats": {"ticker": ORIENT_TICKER, "security_id": int(ids[0]),
                                 "rows_off_calendar": int(np.count_nonzero(~on)),
                                 "duplicate_sessions_quarantined": int(np.count_nonzero(dup)),
                                 "sessions_with_slope": int(np.count_nonzero(np.isfinite(kept)))}}


def kept_slope(h, s: np.ndarray, iv: np.ndarray) -> np.ndarray:
    """The ``SKEW_RULE`` slope of valid cells, f64 (NaN otherwise)."""
    with np.errstate(invalid="ignore"):
        ok = _valid_iv(h, iv) & np.isfinite(s) & (s != 0) & (np.abs(s) <= np.float32(SLOPE_ABS_MAX))
    return np.where(ok, s.astype(np.float64), np.nan)


# ---------------------------------------------------------------------------------------------------------------
# Producers: each returns {name: (writer, sources, extras)}
# ---------------------------------------------------------------------------------------------------------------

def orientation(spy_s: np.ndarray, t: int) -> float:
    """o_t (``SKEW_RULE``): the sign of SPY's median kept slope over rows t-21..t-1, NaN when undefined."""
    lo, hi = t - LAG_SESSIONS - WINDOW + 1, t - LAG_SESSIONS
    if lo < 0 or hi >= len(spy_s):
        return np.nan
    w = spy_s[lo:hi + 1]
    w = w[np.isfinite(w)]
    if len(w) < MIN_DAYS:
        return np.nan
    m = float(np.median(w))
    return float(np.sign(m)) if m != 0.0 else np.nan


def skew_rows(h, panel: dict, role, output: Path, budget) -> dict:
    """``iv_skew_21`` (its field definition)."""
    n, spy_s = role.n, panel["spy"]["s"]
    w = h.FieldWriter(output, "iv_skew_21", role)
    st = {"sessions_oriented_positive": 0, "sessions_oriented_negative": 0, "sessions_unoriented": 0,
          "invalid_member_cells": 0}
    try:
        for t in range(role.n_dates):
            o = orientation(spy_s, t)
            s = t - LAG_SESSIONS
            row = np.full(n, np.nan)
            if np.isfinite(o) and 0 <= s < role.n_dates:
                k = kept_slope(h, panel["skew"][s], panel["iv"][s])
                row = o * k
                st["invalid_member_cells"] += int(np.count_nonzero(~np.isfinite(k) & (role.member[t] != 0)))
            key = ("sessions_oriented_positive" if o > 0 else "sessions_oriented_negative") if np.isfinite(o) \
                else "sessions_unoriented"
            st[key] += 1
            w.write(row)
            if t % 256 == 0:
                budget.check("ivshape-skew-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    return {"iv_skew_21": (w, st)}


def vov_rows(h, panel: dict, role, output: Path, budget) -> dict:
    """``iv_vov_21`` (its field definition)."""
    n, iv = role.n, panel["iv"]
    w = h.FieldWriter(output, "iv_vov_21", role)
    st = {"short_window_member_cells": 0}
    try:
        for t in range(role.n_dates):
            lo, hi = t - LAG_SESSIONS - WINDOW + 1, t - LAG_SESSIONS
            row = np.full(n, np.nan)
            if lo >= 0:
                x = iv[lo:hi + 1]
                ok = _valid_iv(h, x)
                xv = np.where(ok, x.astype(np.float64), 0.0)
                k = ok.sum(axis=0)
                kk = np.maximum(k, 1).astype(np.float64)
                mean = xv.sum(axis=0) / kk
                dev = np.where(ok, xv - mean, 0.0)
                with np.errstate(invalid="ignore", divide="ignore"):
                    sd = np.sqrt((dev * dev).sum(axis=0) / np.maximum(k - 1, 1))
                    row = np.where(k >= MIN_DAYS, sd / mean, np.nan)
                st["short_window_member_cells"] += int(np.count_nonzero((k < MIN_DAYS) & (role.member[t] != 0)))
            else:
                st["short_window_member_cells"] += int(np.count_nonzero(role.member[t] != 0))
            w.write(row)
            if t % 256 == 0:
                budget.check("ivshape-vov-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    return {"iv_vov_21": (w, st)}


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "IvShapeFieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object.
    The plain builder never calls it (draft: prepare_research_fields_ydata.register)."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_ivshape: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return IvShapeFieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """No input pin beyond the role: the vendor file must hash to the role's source_sha256 and the prior is bound to
    the same role; the producers import no other field module's code."""
    return {}


def entry_inputs(entry: dict) -> dict:
    return {}


class IvShapeFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        """No new options: the fields read research_fields_price.py's --price-source (OPTIONS)."""

    @staticmethod
    def check(selected, options: dict):
        """Before any output: the price source and the requested fields' columns (a footer read)."""
        names = [f for f in selected if f in FIELDS]
        if not names:
            return
        if options.get("price_source") is None:
            raise ValueError(f"--fields: {', '.join(names)} need --price-source (the role's vendor source file)")
        require_columns(Path(options["price_source"]), names)

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        if h.SEAL != rw.SEAL:
            raise rw.SealError(f"research_fields_ivshape: the builder's seal {h.SEAL.isoformat()} is not "
                               f"research_window's {rw.SEAL_DATE} ({rw.WINDOW_ID}); refusing (sealed)")
        source = Path(options["price_source"])
        captured = verify_iv_source(h, source, role, budget)
        panel = iv_panel(h, source, captured, role, budget, names)
        budget.report("ivshape-source-loaded", rows_selected=panel["stats"]["rows_selected"])
        results = {}
        if "iv_skew_21" in names:
            results.update(skew_rows(h, panel, role, output, budget))
        if "iv_vov_21" in names:
            results.update(vov_rows(h, panel, role, output, budget))
        check = {"lag_sessions": LAG_SESSIONS, "clock": IV_CLOCK, "seal": rw.SEAL_DATE, "window": rw.WINDOW_ID,
                 "source": panel["stats"]}
        if "spy" in panel:
            check["orientation_line"] = panel["spy"]["stats"]
        source_checks[GROUP] = check
        del panel
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        source_pin = {"path": str(source.resolve()), "bytes": captured[2], "sha256": role.source_sha256}
        for x in names:
            w, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, LAG_SESSIONS)),
                               "lag_sessions": LAG_SESSIONS, "min_history": spec["min_history"], **extra}
            if "domain" in spec:
                field_extras[x]["domain"] = list(spec["domain"])
            outcome[x] = (w, [source_pin], w.coverage())
        budget.report("ivshape-complete", fields=len(names))
