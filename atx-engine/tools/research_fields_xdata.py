"""Expansion-X data-lane research fields (platform v8 lane XDATA): three TickerHistory3 families no v8.0 roster member
reads. Draft and off by default.

``prepare_research_fields.py`` does not register this module, so the plain builder, every v8 field list (fields v9 to
v12), every existing field's formula and every existing producer fingerprint are exactly as before.
``prepare_research_fields_xdata.py`` registers it (``bind`` appends ``FIELDS`` to the builder's ``ALL_FIELDS`` after
every field registered before it; the entry appends the module object to ``FIELD_MODULES``) and runs the builder's own
``main``. No new CLI option: the fields read the role's own vendor source through research_fields_price.py's
``--price-source`` (its SHA-256 must equal the role manifest's ``source_sha256``).

Fields (every one point in time; ``LAG_SESSIONS`` = 1: row t reads nothing dated after session t-1):
* ``div_month_pred``: Hartzmark-Solomon (2013) dividend-month premium. 1 when the line's dividend ledger holds an
  ex-date in calendar month M-3, M-6, M-9 or M-12 (M = the decision session's month), 0 for any other quarterly payer
  (3 to 6 paid months in M-12..M-1); NaN for a non-payer, an annual, semiannual or monthly payer, or a line without a
  full year of history (the 3/6/9/12 lag set predicts a quarterly cycle). The ledger comes from the vendor's chained
  cumulative factor: an observed session whose factor step is a cash-distribution yield in [1 bp, 4%] and not a
  factor-break-v1 jump cell is an ex-date.
* ``beta_dvol_21``: Ang-Hodrick-Xing-Zhang (2006) aggregate-volatility beta. The slope on dIV in the OLS of the line's
  daily adjusted return on [1, r_SPY, dIV_SPY] over the 21 sessions ending t-1 (at least 17 usable), where dIV_SPY is
  the daily change of SPY's vendor 30-day ATM implied volatility (``atmCenI_21d``: the VXO analogue AHXZ use) and r_SPY
  the SPY adjusted return (the value-weighted market proxy).
* ``season_y2_5``: Heston-Sadka (2008) / Keloharju-Linnainmaa-Nyberg (2016) return seasonality, years 2 to 5. The mean
  over k = 2..5 of the adjusted return of the 21-session window that, k years (252 k sessions) ago, matched the next
  21 sessions (the alignment of the roster member ``seasonality_same_month``, which is year 1); at least 3 of the 4
  windows. Not expressible in the DSL: it reads 1,260 sessions back, above the runner's 336.

Stamping (when a value becomes known): the vendor end-of-day row of session s is known at s's 22:00 UTC close mark,
except the implied volatility, which the vendor delivers at 22:00 America/Chicago (03:00-04:00 UTC of the next day;
atx-db TIER1_V3_STATUS "IV clock"). Every field reads rows dated on or before session t-1 only, so even the IV of
session t-1 is delivered (by 04:00 UTC of session t) before the 22:00 UTC mark of t and the 23:00 UTC decision. The
dividend ledger stamps an ex-date at its own session (the vendor applies the factor on the ex-date; the dividend was
declared before it), and only calendar months before the decision month are read.

Seal (reader side, research_window): ``compute`` refuses unless the builder's ``SEAL`` is ``research_window.SEAL``; the
role-line panel (research_fields_price.source_panel) and the SPY reader (``vol_line``) skip and count every vendor row
dated on or after it, and never read past the role's last session.

``--reuse`` (v8 C-3 contract): ``PRODUCERS``, ``HOST_HANDLES``, ``producer_group``, ``field_spec``, ``reuse_inputs``
and ``entry_inputs``; every computed entry records ``producer`` (this module's code identity). Input pins beyond the
role: the NYSE rule calendar of the extended axis (research_fields_price.session_calendar, review B-1) and the code the
producers import from research_fields_price.py (``imported_code``: the AST closure of the imported names with the
builder definitions they read through ``h``), which no producer fingerprint follows. The price source must hash to
the role's source_sha256 and the prior is bound to the same role, so the source needs no further pin.
"""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

import code_fingerprint  # same directory: the AST closure fingerprints --reuse keys on
import research_fields_price as price  # same directory; it does not import this module
import research_window as rw
from research_fields_sec import _Host

GROUP = "xdata"
OPTIONS = ("price_source",)          # research_fields_price.py's option: no new CLI option
HOST_HANDLES = ("h",)                # v8 C-3: the producers read the builder through ``h``
BUILDER = "prepare_research_fields.py"            # the builder these producers are bound into (same directory)
BUILDER_ORCHESTRATION = frozenset({"run", "main"})  # its PRODUCER_ORCHESTRATION (never followed by a fingerprint)
LAG_SESSIONS = 1                     # row t reads rows dated on or before session t - LAG_SESSIONS only
MONTH_SESSIONS_MAX = 23              # NYSE sessions in a calendar month, at most

# ---- div_month_pred (Hartzmark-Solomon 2013) -----------------------------------------------------------------------
DIV_YIELD_MIN = 1e-4                 # 1 bp: above the vendor factor's print precision (1/returnFactor to 1.3e-7)
DIV_YIELD_MAX = 0.04                 # a regular payment above 4% of price is >16% a year: specials, spin-offs and >=5%
#                                      stock dividends (1/1.05 -> 4.76%) fall outside
DIV_PRED_MONTHS = (3, 6, 9, 12)      # HS 2013: a dividend is predicted three, six, nine or twelve months after one
DIV_PAYER_MONTHS = 12                # payer: ex-dates in months M-12..M-1
DIV_MIN_PAID = 3                     # quarterly payer: at least three of its four payments visible (one may be skipped)
DIV_MONTHLY_MAX = 6                  # at most two extra paid months; more is a monthly payer (no off months)
DIV_PRE_SESSIONS = MONTH_SESSIONS_MAX * (DIV_PAYER_MONTHS + 2)

# ---- beta_dvol_21 (Ang-Hodrick-Xing-Zhang 2006) --------------------------------------------------------------------
VOL_LINE_TICKER = "SPY"              # the market line: S&P 500 ETF, its own adjusted return and 30-day ATM IV
VOL_COLUMN = "atmCenI_21d"           # 21 trading days = 30 calendar days (VXO's horizon)
DVOL_WINDOW = 21                     # AHXZ: daily returns within one month
DVOL_MIN_DAYS = 17                   # declared: about 80% of the window, as F-1's 48 of 60 months
DVOL_DET_TOL = 1e-10                 # |corr(r_SPY, dIV)| >= 1 - 5e-11 on a line's days: collinear -> NaN
DVOL_PRE_SESSIONS = DVOL_WINDOW + 1

# ---- season_y2_5 (Heston-Sadka 2008; Keloharju-Linnainmaa-Nyberg 2016) --------------------------------------------
SEASON_YEARS = (2, 3, 4, 5)
SEASON_YEAR_SESSIONS = 252           # one year, as seasonality_same_month (delay 252 / delay 231)
SEASON_WINDOW = 21
SEASON_MIN_YEARS = 3                 # declared: a majority of the four windows (a halt or listing gap drops one)
SEASON_PRE_SESSIONS = SEASON_YEAR_SESSIONS * max(SEASON_YEARS)

SOURCE_NOTE = ("vendor observations of the role's lines on the extended axis (NYSE rule sessions before the role, the "
               "role's sessions inside it): research_fields_price SOURCE_RULE (unique positive key, finite positive "
               "close and cumulReturnFactor, finite volume >= 0; F = the chained factor with every factor-break-v1 "
               "repaired step divided out; a span across a kept_gap step is unusable)")
LINE_NOTE = "one price line's values (the vendor securityID), not an issuer total"

DIV_CLOCK = ("xdata-div-month-v1: row t reads the dividend ledger of calendar months before the month of session t "
             "only (every session of those months is at or before t-1; each ex-date row known at its 22:00 UTC close "
             "mark)")
DVOL_CLOCK = ("xdata-dvol-lag1-v1: row t reads vendor rows of sessions t-22..t-1 only (line closes and factors at "
              "their 22:00 UTC close mark; SPY's atmCenI_21d of session s delivered by 04:00 UTC of s+1, before the "
              "22:00 UTC mark of t)")
SEASON_CLOCK = ("xdata-season-y2-5-v1: row t reads vendor rows of sessions t-1260..t-483 only (each known at its "
                "22:00 UTC close mark)")

DIV_RULE = (
    "hs-divseason-q3-6-9-12-v1. Ledger: for each line and each observed extended session s with an earlier "
    "observation p (the line's previous one), y = 1 - F_p / F_s; s is an ex-date iff "
    f"{DIV_YIELD_MIN} <= y <= {DIV_YIELD_MAX}, no kept_gap step lies in (p, s], and (p, s) is not a factor-break-v1 "
    "jump cell (|ln F_s/F_p| > 0.01 and |ln P_s/P_p| > |ln C_s/C_p| + 0.01, P = C F: a factor step with no matching "
    "raw price drop). Row t: M = the calendar month of session t; paid(m) = an ex-date in month m at an extended row "
    f"<= t - {LAG_SESSIONS}; n_paid = the number of months m in M-{DIV_PAYER_MONTHS}..M-1 with paid(m); a quarterly "
    f"payer has {DIV_MIN_PAID} <= n_paid <= {DIV_MONTHLY_MAX} and its first observation at or before the first session "
    f"of month M-12; value = 1 if paid(M-k) for some k in {DIV_PRED_MONTHS}, else 0, for a quarterly payer; NaN "
    "otherwise (non-payers, annual and semiannual payers, monthly payers, short history)")
DVOL_RULE = (
    "ahxz-beta-dvol-spy21-v1. Market line: the unique vendor securityID whose ticker_tk is 'SPY' on every row read "
    "(another securityID with that ticker -> refused). Per extended session s: r_SPY(s) = P_s / P_(s-1) - 1 of the SPY "
    "line (observations at s and the previous extended session; excluded when |ln P ratio| > 1.5, > |ln C ratio| + "
    "0.10, or a factor-break-v1 jump cell); dIV(s) = IV_s - IV_(s-1), IV = atmCenI_21d kept iff float32(0.02) <= v <= "
    "float32(5.0) (IV_DOMAIN). Line return r(s) = P_s / P_(s-1) - 1 with observations at s and s-1, no kept_gap step "
    "in (s-1, s] and the house guard (|ln P ratio| <= 1.5 and <= |ln C ratio| + 0.10). Row t: the days s in "
    f"t-{DVOL_WINDOW + LAG_SESSIONS - 1}..t-{LAG_SESSIONS} with r, r_SPY and dIV finite (K days); OLS r = a + b1 "
    f"r_SPY + b2 dIV on them; value = b2 when K >= {DVOL_MIN_DAYS} and the two regressors are not collinear "
    f"(det > {DVOL_DET_TOL} s11 s22 of their centred cross-products), else NaN. Raw returns (no risk-free rate: a "
    "constant shift moves only the intercept)")
SEASON_RULE = (
    "hs-season-y2-5-v1. Row t, e = its extended row: for k in 2..5, b = e - 252 k, a = b + 21; r_k = P_a / P_b - 1 "
    "with observations at a and b, no kept_gap step in (b, a], |ln P_a/P_b| <= |ln C_a/C_b| + 0.10 (the coskew "
    f"monthly guard) and a <= e - {LAG_SESSIONS}; value = the mean of the finite r_k when at least {SEASON_MIN_YEARS} "
    "of the 4 are finite, else NaN")

FIELDS: dict = {}


def _spec(name, group, units, definition, staleness, source_columns, caveats, formula, min_history, clock,
          domain=None):
    spec = {"group": group, "point_in_time": True, "lagged": False, "units": units, "clock": clock,
            "staleness": staleness, "caveats": caveats, "definition": definition, "source_columns": source_columns,
            "formula_id": formula, "min_history": min_history, "needs": "vendor_prices"}
    if domain is not None:
        spec["domain"] = domain
    FIELDS[name] = spec


_PRICE_COLS = ["close", "cumulReturnFactor", "volume", "tradingDate", "securityID"]
_spec("div_month_pred", "x_div",
      "indicator among quarterly dividend payers: 1 in a predicted dividend month (an ex-date 3, 6, 9 or 12 months "
      "earlier)",
      DIV_RULE + ". " + SOURCE_NOTE,
      "event ledger, no fill: a non-payer, an annual, semiannual or monthly payer, or a line first observed after month "
      "M-12 begins -> NaN",
      _PRICE_COLS,
      ["the ex-date ledger is read from the vendor cumulative factor, not a dividend file: a cash distribution of 1 bp "
       "to 4% of the prior close counts whatever its kind (regular, special inside the band, return of capital, a "
       "small spin-off); a vendor factor that misdates the ex-date by a session can move it across a month end",
       "the vendor history carries no vintage proof: a dividend factor added later to the history would be seen as "
       "if known at its ex-date (historical_vintage_verified false)",
       "a payer's 0 means 'no predicted ex-date this month', not 'no dividend'", LINE_NOTE],
      "hs-divseason-q3-6-9-12-v1",
      f"{DIV_PRE_SESSIONS} extended sessions before the role start (14 calendar months)", DIV_CLOCK, domain=(0.0, 1.0))
_spec("beta_dvol_21", "x_dvol",
      "return per unit of daily IV change: slope on SPY's 30-day ATM implied-volatility change (AHXZ 2006)",
      DVOL_RULE + ". " + SOURCE_NOTE,
      f"no fill: fewer than {DVOL_MIN_DAYS} usable days in the window -> NaN",
      _PRICE_COLS + ["ticker_tk", VOL_COLUMN],
      ["the volatility factor is SPY's vendor ATM IV change (earnings-censored atmCenI; SPY has no earnings), not the "
       "Cboe VIX; AHXZ use VXO (S&P 100 ATM), the closer analogue",
       "the market regressor is SPY's adjusted return (dividends included), not the CRSP value-weighted index",
       "21 sessions anchored at t-1 and moving daily, not calendar months", LINE_NOTE],
      "ahxz-beta-dvol-spy21-v1", f"{DVOL_PRE_SESSIONS} extended sessions before the role start", DVOL_CLOCK)
_spec("season_y2_5", "x_season",
      "simple return, decimal: mean same-season 21-session return of years 2 to 5",
      SEASON_RULE + ". " + SOURCE_NOTE,
      f"no fill: fewer than {SEASON_MIN_YEARS} usable annual windows -> NaN",
      _PRICE_COLS,
      ["years are 252 sessions and windows 21 sessions anchored at the decision row (the house seasonality "
       "alignment), not calendar months", "TickerHistory3 starts 2012-03-26: a window before it is missing",
       LINE_NOTE],
      "hs-season-y2-5-v1", f"{SEASON_PRE_SESSIONS} extended sessions before the role start", SEASON_CLOCK)

PRE_SESSIONS = {"div_month_pred": DIV_PRE_SESSIONS, "beta_dvol_21": DVOL_PRE_SESSIONS,
                "season_y2_5": SEASON_PRE_SESSIONS}
# The producing code of each field group (contract of task C-3: {group: (entry names,)}); compute() orchestrates.
PRODUCERS = {"x_div": ("div_events", "div_month_rows"),
             "x_dvol": ("vol_line", "line_returns", "dvol_beta_rows"),
             "x_season": ("season_rows",)}
# The names the producers (and compute) call from research_fields_price.py: their closure is an input pin.
PRICE_IMPORTS = ("source_panel", "verify_source", "extended_days", "Gaps", "_Writers", "GUARD_LOG", "GUARD_EXCESS")


# ---------------------------------------------------------------------------------------------------------------
# Imported code: the --reuse input pin of what the producers import
# ---------------------------------------------------------------------------------------------------------------

def imported_code(group: str, source: bytes | None = None, builder: bytes | None = None) -> dict:
    """{"module", "names", "sha256"}: SHA-256 (code_fingerprint) of the AST closure of ``PRICE_IMPORTS`` in
    research_fields_price.py, plus the closure of every builder definition they read through ``h``. ``source`` /
    ``builder`` replace the files read (tests). The same value for every group: they share the panel."""
    if group not in PRODUCERS:
        raise ValueError(f"research_fields_xdata: unknown producer group {group!r}")
    path = Path(price.__file__)
    src = path.read_bytes() if source is None else source
    host = (Path(__file__).resolve().parent / BUILDER).read_bytes() if builder is None else builder
    fp = code_fingerprint.fingerprints(src.replace(b"\r\n", b"\n"), {group: PRICE_IMPORTS},
                                       host=code_fingerprint.Host(host.replace(b"\r\n", b"\n"), price.HOST_HANDLES,
                                                                  BUILDER_ORCHESTRATION))[group]
    if fp is None:
        raise ValueError(f"research_fields_xdata: {path.name} lacks one of {PRICE_IMPORTS}")
    return {"module": path.name, "names": list(PRICE_IMPORTS), "sha256": fp}


# ---------------------------------------------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------------------------------------------

def require_vol_columns(h, path: Path) -> None:
    """Refuse (a footer read, before any output) a price source without SPY's inputs of ``beta_dvol_21``."""
    schema = pq.ParquetFile(Path(path), memory_map=False).schema_arrow
    for c, t in (("ticker_tk", h.SV_TH_TYPES["ticker_tk"]), (VOL_COLUMN, h.TH_TYPES[VOL_COLUMN])):
        i = schema.get_field_index(c)
        if i < 0 or schema.field(i).type != t:
            raise ValueError(f"beta_dvol_21: --price-source column {c} is missing or not {t}")


def vol_line(h, path: Path, verified, days: np.ndarray, budget) -> dict:
    """SPY's daily adjusted return ``m`` and ATM IV change ``dv`` on the extended sessions ``days`` (``DVOL_RULE``).
    ``verified`` is research_fields_price.verify_source's result; the file must be unchanged since."""
    path = Path(path)
    captured = verified[0]
    pf = pq.ParquetFile(path, memory_map=False)
    columns = ["tradingDate", "securityID", "ticker_tk", "close", "cumulReturnFactor", "volume", VOL_COLUMN]
    require_vol_columns(h, path)
    seal_day = h.day_of(h.SEAL)
    first, last = int(days[0]), min(int(days[-1]), seal_day - 1)
    st: dict = {"ticker": VOL_LINE_TICKER, "column": VOL_COLUMN, "rows_selected": 0,
                "rows_on_or_after_seal_skipped": 0, "rows_nonpositive_id": 0, "rows_off_calendar": 0}
    got: dict = {k: [] for k in ("d", "sid", "close", "f", "volume", "iv")}
    for batch in pf.iter_batches(batch_size=65536, columns=columns, use_threads=False):
        budget.check("xdata-vol-batch")
        hit = pc.fill_null(pc.equal(batch.column("ticker_tk"), VOL_LINE_TICKER), False).to_numpy(zero_copy_only=False)
        if not hit.any():
            continue
        d = pc.fill_null(batch.column("tradingDate").cast(pa.int32()), -1).to_numpy().astype(np.int64)
        st["rows_on_or_after_seal_skipped"] += int(np.count_nonzero(hit & (d >= seal_day)))
        sid = pc.fill_null(batch.column("securityID"), 0).to_numpy()
        st["rows_nonpositive_id"] += int(np.count_nonzero(hit & (sid <= 0)))
        idx = np.flatnonzero(hit & (d >= first) & (d <= last) & (sid > 0))
        if not len(idx):
            continue
        sub = batch.take(pa.array(idx, type=pa.int64()))
        got["d"].append(d[idx])
        got["sid"].append(sid[idx])
        for k, c in (("close", "close"), ("f", "cumulReturnFactor"), ("volume", "volume"), ("iv", VOL_COLUMN)):
            got[k].append(sub.column(c).to_numpy(zero_copy_only=False).astype(np.float64))
    if h.identity(path) != captured:
        raise ValueError("--price-source changed while the SPY line was read")
    cat = {k: np.concatenate(v) if v else np.zeros(0) for k, v in got.items()}
    ids = np.unique(cat["sid"].astype(np.int64))
    if len(ids) != 1:
        raise ValueError(f"beta_dvol_21: ticker {VOL_LINE_TICKER} maps to {len(ids)} vendor securityIDs "
                         f"({', '.join(str(int(x)) for x in ids[:5])}) between {h.date_of(first)} and "
                         f"{h.date_of(last)}; the market line must be unique")
    n_ext = len(days)
    e = np.minimum(np.searchsorted(days, cat["d"].astype(np.int64)), n_ext - 1)
    on = days[e] == cat["d"]
    st["rows_off_calendar"] = int(np.count_nonzero(~on))
    e = e[on]
    copies = np.bincount(e, minlength=n_ext)
    C, F, IV = (np.full(n_ext, np.nan) for _ in range(3))
    with np.errstate(invalid="ignore"):
        close, f, vol, iv = (cat[k][on] for k in ("close", "f", "volume", "iv"))
        obs = np.isfinite(close) & (close > 0) & np.isfinite(f) & (f > 0) & np.isfinite(vol) & (vol >= 0)
        iv32 = iv.astype(np.float32)
        lo, hi = h.IV_DOMAIN
        in_dom = np.isfinite(iv32) & (iv32 >= np.float32(lo)) & (iv32 <= np.float32(hi))
    C[e] = np.where(obs, close, np.nan)
    F[e] = np.where(obs, f, np.nan)
    IV[e] = np.where(obs & in_dom, iv32.astype(np.float64), np.nan)
    dup = copies > 1
    C[dup], F[dup], IV[dup] = np.nan, np.nan, np.nan
    st["rows_selected"] = int(len(e))
    st["duplicate_sessions_quarantined"] = int(np.count_nonzero(dup))
    st["iv_missing"] = int(np.count_nonzero(obs & ~np.isfinite(iv32)))
    st["iv_out_of_domain"] = int(np.count_nonzero(obs & np.isfinite(iv32) & ~in_dom))
    m, dv = np.full(n_ext, np.nan), np.full(n_ext, np.nan)
    with np.errstate(invalid="ignore", divide="ignore"):
        r = np.log(C[1:]) - np.log(C[:-1])
        s = np.log(F[1:]) - np.log(F[:-1])
        a = r + s
        kept = (np.abs(a) <= price.GUARD_LOG) & (np.abs(a) <= np.abs(r) + price.GUARD_EXCESS)
        jump = (np.abs(s) > h.FB_CELL_STEP) & (np.abs(a) > np.abs(r) + h.FB_CELL_EXCESS)
        m[1:] = np.where(np.isfinite(a) & kept & ~jump, np.expm1(a), np.nan)
        dv[1:] = IV[1:] - IV[:-1]
    st.update({"security_id": int(ids[0]), "sessions_with_return": int(np.count_nonzero(np.isfinite(m))),
               "sessions_with_dvol": int(np.count_nonzero(np.isfinite(dv))),
               "returns_excluded": int(np.count_nonzero(np.isfinite(a) & ~(kept & ~jump)))})
    return {"m": m, "dv": dv, "stats": st}


# ---------------------------------------------------------------------------------------------------------------
# Producers: each returns {name: (writer, sources, extras)}
# ---------------------------------------------------------------------------------------------------------------

def div_events(h, panel: dict) -> tuple:
    """The ex-date ledger (``DIV_RULE``): (event mask n_ext x n, first observed row per line, stats)."""
    F, C, gaps = panel["F"], panel["C"], panel["gaps"]
    n_ext, n = F.shape
    cols = np.arange(n)
    ev = np.zeros((n_ext, n), dtype=bool)
    last = np.full(n, -1, dtype=np.int64)
    first = np.full(n, n_ext, dtype=np.int64)
    st = {"event_steps": 0, "steps_above_max_yield": 0, "jump_steps_excluded": 0, "kept_gap_steps_excluded": 0}
    for s in range(n_ext):
        obs = np.isfinite(F[s]) & np.isfinite(C[s])
        have = obs & (last >= 0)
        if have.any():
            p = np.maximum(last, 0)
            fp, cp = F[p, cols], C[p, cols].astype(np.float64)
            with np.errstate(invalid="ignore", divide="ignore"):
                y = 1.0 - fp / F[s]
                sf = np.log(F[s]) - np.log(fp)
                r = np.log(C[s].astype(np.float64)) - np.log(cp)
                jump = (np.abs(sf) > h.FB_CELL_STEP) & (np.abs(r + sf) > np.abs(r) + h.FB_CELL_EXCESS)
                band = (y >= DIV_YIELD_MIN) & (y <= DIV_YIELD_MAX)
            crossed = gaps.crosses(p, s, (n,))
            ev[s] = have & band & ~jump & ~crossed
            st["event_steps"] += int(np.count_nonzero(ev[s]))
            st["steps_above_max_yield"] += int(np.count_nonzero(have & (y > DIV_YIELD_MAX)))
            st["jump_steps_excluded"] += int(np.count_nonzero(have & band & jump))
            st["kept_gap_steps_excluded"] += int(np.count_nonzero(have & band & ~jump & crossed))
        first = np.where(obs & (first == n_ext), s, first)
        last = np.where(obs, s, last)
    st["lines_with_events"] = int(np.count_nonzero(ev.any(axis=0)))
    return ev, first, st


def div_month_rows(h, panel: dict, role, output: Path, budget) -> dict:
    """``div_month_pred`` (its field definition)."""
    n, pre, days = role.n, panel["prefix"], panel["days"]
    n_ext = len(days)
    budget.admit(n_ext * n * 5 + (16 << 20), "xdata-div-ledger")
    ev, first_obs, st = div_events(h, panel)
    cum = np.cumsum(ev, axis=0, dtype=np.int32)
    del ev
    mon = days.astype("datetime64[D]").astype("datetime64[M]").astype(np.int64)
    zero = np.zeros(n, dtype=np.int32)
    need = sorted({k for k in range(1, DIV_PAYER_MONTHS + 1)} | set(DIV_PRED_MONTHS))
    ws = price._Writers(h, output, ["div_month_pred"], role)
    w = ws.w["div_month_pred"]
    counts = {"payer_member_cells": 0, "predicted_member_cells": 0, "nonpayer_member_cells": 0,
              "infrequent_payer_member_cells": 0, "monthly_payer_member_cells": 0, "short_history_member_cells": 0}
    try:
        for t in range(role.n_dates):
            e = pre + t
            month = int(mon[e])
            vis = min(e - LAG_SESSIONS, n_ext - 1)
            row = np.full(n, np.nan)
            member = role.member[t] != 0
            if month - max(need) > int(mon[0]):     # the axis's first month may be partial: never read
                paid = {}
                for k in need:
                    m = month - k
                    lo = int(np.searchsorted(mon, m, side="left"))
                    hi = min(int(np.searchsorted(mon, m, side="right")) - 1, vis)
                    paid[k] = (cum[hi] - (cum[lo - 1] if lo > 0 else zero)) > 0 if lo <= hi else zero > 0
                n_paid = np.sum([paid[k] for k in range(1, DIV_PAYER_MONTHS + 1)], axis=0)
                pred = np.any([paid[k] for k in DIV_PRED_MONTHS], axis=0)
                history = first_obs <= int(np.searchsorted(mon, month - DIV_PAYER_MONTHS, side="left"))
                ok = history & (n_paid >= DIV_MIN_PAID) & (n_paid <= DIV_MONTHLY_MAX)
                row = np.where(ok, pred.astype(np.float64), np.nan)
                counts["payer_member_cells"] += int(np.count_nonzero(ok & member))
                counts["predicted_member_cells"] += int(np.count_nonzero(ok & pred & member))
                counts["nonpayer_member_cells"] += int(np.count_nonzero(history & (n_paid == 0) & member))
                counts["infrequent_payer_member_cells"] += int(np.count_nonzero(history & (n_paid > 0)
                                                                                & (n_paid < DIV_MIN_PAID) & member))
                counts["monthly_payer_member_cells"] += int(np.count_nonzero(history & (n_paid > DIV_MONTHLY_MAX)
                                                                             & member))
                counts["short_history_member_cells"] += int(np.count_nonzero(~history & member))
            else:
                counts["short_history_member_cells"] += int(np.count_nonzero(member))
            w.write(row)
            if t % 256 == 0:
                budget.check("xdata-div-write")
    except BaseException:
        ws.abort()
        raise
    ws.close()
    return {"div_month_pred": (w, [panel["source"]], {"domain": [0.0, 1.0], "ledger": st, **counts})}


def line_returns(h, panel: dict, lo: int) -> np.ndarray:
    """Daily adjusted returns of the role lines on extended rows lo..n_ext-1 (row s - lo; ``DVOL_RULE``): consecutive
    observations, no kept_gap step in (s-1, s], the house guard."""
    F, C, gaps = panel["F"], panel["C"], panel["gaps"]
    n_ext, n = F.shape
    lo = max(int(lo), 1)
    out = np.full((max(n_ext - lo, 0), n), np.nan)
    for s in range(lo, n_ext):
        with np.errstate(invalid="ignore", divide="ignore"):
            ratio = (C[s].astype(np.float64) * F[s]) / (C[s - 1].astype(np.float64) * F[s - 1])
            la = np.log(ratio)
            lr = np.log(C[s].astype(np.float64)) - np.log(C[s - 1].astype(np.float64))
            ok = np.isfinite(la) & (np.abs(la) <= price.GUARD_LOG) & (np.abs(la) <= np.abs(lr) + price.GUARD_EXCESS)
        ok &= ~gaps.crosses(s - 1, s, (n,))
        out[s - lo] = np.where(ok, ratio - 1.0, np.nan)
    return out


def dvol_beta_rows(h, panel: dict, vol: dict, role, output: Path, budget) -> dict:
    """``beta_dvol_21`` (its field definition) from the panel and SPY's ``vol_line``."""
    n, pre, n_ext = role.n, panel["prefix"], len(panel["days"])
    lo0 = max(1, pre - LAG_SESSIONS - DVOL_WINDOW + 1)
    budget.admit((max(n_ext - lo0, 0) + 12 * DVOL_WINDOW) * n * 8 + (16 << 20), "xdata-dvol-returns")
    R = line_returns(h, panel, lo0)
    mk, dv = vol["m"], vol["dv"]
    ws = price._Writers(h, output, ["beta_dvol_21"], role)
    w = ws.w["beta_dvol_21"]
    short = 0
    try:
        for t in range(role.n_dates):
            end = pre + t - LAG_SESSIONS
            lo = end - DVOL_WINDOW + 1
            row = np.full(n, np.nan)
            if lo >= lo0 and end < n_ext:
                y = R[lo - lo0:end - lo0 + 1]
                x1, x2 = mk[lo:end + 1], dv[lo:end + 1]
                wt = np.isfinite(y) & (np.isfinite(x1) & np.isfinite(x2))[:, None]
                k = wt.sum(axis=0)
                kk = np.maximum(k, 1).astype(np.float64)
                X1 = np.where(wt, np.nan_to_num(x1)[:, None], 0.0)
                X2 = np.where(wt, np.nan_to_num(x2)[:, None], 0.0)
                Y = np.where(wt, y, 0.0)
                d1 = (X1 - X1.sum(axis=0) / kk) * wt
                d2 = (X2 - X2.sum(axis=0) / kk) * wt
                dy = (Y - Y.sum(axis=0) / kk) * wt
                s11, s22, s12 = (d1 * d1).sum(axis=0), (d2 * d2).sum(axis=0), (d1 * d2).sum(axis=0)
                s1y, s2y = (d1 * dy).sum(axis=0), (d2 * dy).sum(axis=0)
                det = s11 * s22 - s12 * s12
                with np.errstate(invalid="ignore", divide="ignore"):
                    beta = (s11 * s2y - s12 * s1y) / det
                good = (k >= DVOL_MIN_DAYS) & (det > DVOL_DET_TOL * s11 * s22) & np.isfinite(beta)
                short += int(np.count_nonzero((k < DVOL_MIN_DAYS) & (role.member[t] != 0)))
                row = np.where(good, beta, np.nan)
            w.write(row)
            if t % 256 == 0:
                budget.check("xdata-dvol-write")
    except BaseException:
        ws.abort()
        raise
    ws.close()
    return {"beta_dvol_21": (w, [panel["source"]], {"short_window_member_cells": short,
                                                     "vol_line_security_id": vol["stats"]["security_id"]})}


def season_rows(h, panel: dict, role, output: Path, budget) -> dict:
    """``season_y2_5`` (its field definition)."""
    n, pre = role.n, panel["prefix"]
    F, C, gaps = panel["F"], panel["C"], panel["gaps"]
    n_ext = F.shape[0]
    ws = price._Writers(h, output, ["season_y2_5"], role)
    w = ws.w["season_y2_5"]
    short = 0
    try:
        for t in range(role.n_dates):
            e = pre + t
            vis = e - LAG_SESSIONS
            rs = np.full((len(SEASON_YEARS), n), np.nan)
            for i, k in enumerate(SEASON_YEARS):
                b = e - SEASON_YEAR_SESSIONS * k
                a = b + SEASON_WINDOW
                if b < 0 or a > vis or a >= n_ext:
                    continue
                with np.errstate(invalid="ignore", divide="ignore"):
                    ratio = (C[a].astype(np.float64) * F[a]) / (C[b].astype(np.float64) * F[b])
                    la = np.log(ratio)
                    lr = np.log(C[a].astype(np.float64)) - np.log(C[b].astype(np.float64))
                    ok = np.isfinite(la) & (np.abs(la) <= np.abs(lr) + price.GUARD_EXCESS)
                ok &= ~gaps.crosses(b, a, (n,))
                rs[i] = np.where(ok, ratio - 1.0, np.nan)
            fin = np.isfinite(rs)
            cnt = fin.sum(axis=0)
            with np.errstate(invalid="ignore", divide="ignore"):
                mean = np.where(fin, rs, 0.0).sum(axis=0) / np.maximum(cnt, 1)
            short += int(np.count_nonzero((cnt < SEASON_MIN_YEARS) & (role.member[t] != 0)))
            w.write(np.where(cnt >= SEASON_MIN_YEARS, mean, np.nan))
            if t % 256 == 0:
                budget.check("xdata-season-write")
    except BaseException:
        ws.abort()
        raise
    ws.close()
    return {"season_y2_5": (w, [panel["source"]], {"short_history_member_cells": short})}


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "XdataFieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object.
    The plain builder never calls it (draft: prepare_research_fields_xdata.register)."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_xdata: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return XdataFieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """This run's input pins of a field beyond the role: the rule calendar of the extended axis (review B-1) and the
    code its producer imports from research_fields_price.py (imported_code)."""
    return {"session_calendar": price.session_calendar(), "imported_code": imported_code(producer_group(name))}


def entry_inputs(entry: dict) -> dict:
    """The same pins as a manifest entry records them."""
    return {"session_calendar": entry.get("session_calendar"), "imported_code": entry.get("imported_code")}


class XdataFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        """No new options: the fields read research_fields_price.py's --price-source (OPTIONS)."""

    def check(self, selected, options: dict):
        """Before any output: the price source, and SPY's columns for beta_dvol_21 (a footer read)."""
        names = [f for f in selected if f in FIELDS]
        if not names:
            return
        if options.get("price_source") is None:
            raise ValueError(f"--fields: {', '.join(names)} need --price-source (the role's vendor source file)")
        if "beta_dvol_21" in names:
            require_vol_columns(self.h, Path(options["price_source"]))

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        if h.SEAL != rw.SEAL:
            raise rw.SealError(f"research_fields_xdata: the builder's seal {h.SEAL.isoformat()} is not research_window's "
                               f"{rw.SEAL_DATE} ({rw.WINDOW_ID}); refusing (sealed)")
        source = Path(options["price_source"])
        verified = price.verify_source(h, source, role, budget)
        pre = max(PRE_SESSIONS[x] for x in names)
        days, prefix = price.extended_days(role, pre)
        st = {"lag_sessions": LAG_SESSIONS, "seal": rw.SEAL_DATE, "window": rw.WINDOW_ID}
        vol = None
        if "beta_dvol_21" in names:   # before the panel: one small line, released before the matrices exist
            vol = vol_line(h, source, verified, days, budget)
            st["vol_line"] = vol["stats"]
        panel = price.source_panel(h, source, verified, role, budget, pre_sessions=pre, lookback_days=0,
                                   shares=False, open_=False)
        if panel["prefix"] != prefix or not np.array_equal(panel["days"], days):
            raise ValueError("internal: the panel's extended axis differs from the SPY line's")
        st["source"] = panel["stats"]
        budget.report("xdata-source-loaded", rows_selected=panel["stats"]["rows_selected"])
        results = {}
        if "div_month_pred" in names:
            results.update(div_month_rows(h, panel, role, output, budget))
        if vol is not None:
            results.update(dvol_beta_rows(h, panel, vol, role, output, budget))
        if "season_y2_5" in names:
            results.update(season_rows(h, panel, role, output, budget))
        del panel
        source_checks[GROUP] = st
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        calendar = price.session_calendar()
        for x in names:
            w, sources, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, LAG_SESSIONS)),
                               "lag_sessions": LAG_SESSIONS, "min_history": spec["min_history"], **extra,
                               "session_calendar": dict(calendar), "imported_code": imported_code(spec["group"])}
            outcome[x] = (w, sources, w.coverage())
        budget.report("xdata-complete", fields=len(names))
