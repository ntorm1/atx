"""Price-derived and long-lookback point-in-time research fields (platform v8 F-1).

An opt-in field module of ``prepare_research_fields.py``, registered like ``research_fields_sec.py``: the builder's
``FIELD_MODULES`` hook binds it to the builder's own namespace (``bind(globals())``), appends ``FIELDS`` to the registry
after every existing field, and calls ``check`` before any output and ``compute`` after the built-in groups. Nothing
else in the builder changes, so every other field's bytes and manifest entry are identical whether or not these
fields are requested.

Fields (every one point in time; ``LAG_SESSIONS`` = 1: row t reads nothing dated after session t-1):
* ``ret_overnight`` / ``ret_intraday``: the adjusted open split of session t-1's return. The research projection does
  not carry the open (``prepare_recent_research.COLUMNS``); these read it from the role's own vendor source file when
  its schema has an ``open`` column, and otherwise refuse with ``FieldNeedsOpen`` ("field needs open; absent in
  export") before any output.
* ``ceq_iss_5y``: Daniel-Titman (2006) composite equity issuance over 1,260 sessions.
* ``coskew_60m``: Harvey-Siddique (2000) standardised coskewness of 60 monthly (21-session) returns with the
  equal-weight vendor market.
* ``vol_126``: mean daily share volume over the 126 role sessions before t (role ``volume.f64``).
* ``xrd0_ttm``: this run's ``xrd_ttm`` with a missing value set to 0 where this run's ``sale_ttm`` is finite.

Long history (``--price-source``, the vendor TickerHistory3 parquet the role was projected from: its SHA-256 must equal
the role manifest's ``source_sha256``). The role's lines are read on an extended session axis: the NYSE rule sessions
(``research_fields_sec.nyse_sessions``) before the role, the role's own sessions inside it, reaching 1,261 sessions
before the role start (plus the 490-day share lookback for ``ceq_iss_5y``). An observation is the role's present
contract (unique positive (date, id) key, finite positive close and cumulReturnFactor, finite volume >= 0). The vendor
cumulReturnFactor is chained with the builder's ``factor_breaks`` (rule factor-break-v1, as ``shares_out``): every
repaired step is divided out, and a span across a kept_gap step is NaN. Rows dated after the role's last session or on
or after the builder's seal are never used.

``--reuse`` (v8 C-3 contract; Ruling E-21): ``PRODUCERS``, ``HOST_HANDLES``, ``producer_group``, ``field_spec``,
``reuse_inputs`` and ``entry_inputs``; every computed entry records ``producer`` (this module's code identity). The
inputs beyond the role are pinned by the role itself: ``--price-source`` must hash to the role's ``source_sha256``
(and the prior is bound to the same role), and ``xrd0_ttm``'s required fields must be reused with it.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import math
from pathlib import Path
import shutil
import sys

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from research_fields_sec import _Host, nyse_sessions  # the SEC module's calendar and host view (same directory)

GROUP = "price"
OPTIONS = ("price_source",)
HOST_HANDLES = ("h",)                 # v8 C-3: the producers read the builder through ``h``
LAG_SESSIONS = 1
CEQ_SESSIONS = 1260                   # five years of sessions (Daniel-Titman 2006: 60 months)
CEQ_DOMAIN = (-math.log(100.0), math.log(100.0))   # outside: a vendor share-units defect (1000x = 6.9), counted
MONTH_SESSIONS = 21
COSKEW_MONTHS = 60                    # Harvey-Siddique (2000): 60 monthly returns
COSKEW_MIN_MONTHS = 48                # declared with the field: 80% of the window
VOL_WINDOW = 126
VOL_MIN_SESSIONS = 63                 # declared with the field: half the window present
GUARD_LOG = 1.5                       # the house rough_return guard (mkt_ret; strategy_target_replay.cpp)
GUARD_EXCESS = 0.10
OPEN_TYPES = (pa.float32(), pa.float64())
HISTORY_SESSIONS = CEQ_SESSIONS + 1   # sessions before the role that row 0 reaches (t-1 and t-1-1260)
NEVER = np.iinfo(np.int64).max


class FieldNeedsOpen(ValueError):
    """A requested field needs the vendor open, which the price source does not carry."""


PRICE_CLOCK = (
    "price-close-lag1-v1: row t reads vendor observations dated on or before session t-1 only (each known at that "
    "session's 22:00 UTC close mark, before the t-1 decision); sessions are the role's own inside the role and the "
    "NYSE rule sessions (nyse-rule-v1) before it; rows after the role's last session or on or after the seal are "
    "never read")
ROLE_CLOCK = ("role-close-lag1-v1: row t reads the role's volume.f64 and present.u8 rows of sessions t-126..t-1 only "
              "(each known at its 22:00 UTC close mark)")
RD_CLOCK = ("the clock of this run's xrd_ttm and sale_ttm (issuer fundamentals, --fund-lag-sessions): row t reads "
            "their row t only")
SOURCE_RULE = (
    "--price-source = the vendor TickerHistory3 parquet whose SHA-256 is the role manifest's source_sha256; the role's "
    "lines on the extended axis (NYSE rule sessions before the role, the role's sessions inside it); observation = "
    "unique positive (tradingDate, securityID) key with finite positive close and cumulReturnFactor and finite volume "
    ">= 0 (duplicate keys quarantined); adjusted price P = close x F, F = cumulReturnFactor with every factor-break-v1 "
    "repaired step divided out (prepare_research_fields.factor_breaks, as shares_out); a span across a kept_gap step "
    "-> NaN")
MARKET_RULE = (
    "vendor equal-weight market, daily: on session s, the mean of P_s/P_prev - 1 over every vendor line (securityID "
    "> 0, observation contract as SOURCE_RULE) observed on s and on the previous extended session, excluding guarded "
    "cells (|log adj ratio| > 1.5 or > |log raw ratio| + 0.10) and factor-break-v1 jump cells (|log F ratio| > 0.01 "
    "and |log adj ratio| > |log raw ratio| + 0.01); summed with DuckDB fsum on one thread; no contributor -> NaN. "
    "Monthly market return = the product of (1 + daily) over the month's 21 sessions - 1 (daily rebalanced, as the "
    "CRSP EW index); any NaN day -> NaN month")
LINE_NOTE = "one price line's values (the vendor securityID), not an issuer total"

FIELDS = {}


def _spec(name, family, units, definition, staleness, source_columns, caveats, needs, formula, min_history,
          requires=None, domain=None, clock=PRICE_CLOCK):
    spec = {"group": family, "point_in_time": True, "lagged": False, "units": units, "clock": clock,
            "staleness": staleness, "caveats": caveats, "definition": definition, "source_columns": source_columns,
            "formula_id": formula, "min_history": min_history, "needs": needs}
    if requires:
        spec["requires"] = requires
    if domain:
        spec["domain"] = domain
    FIELDS[name] = spec


_OPEN_COLS = ["open", "close", "cumulReturnFactor", "volume", "tradingDate", "securityID"]
_OPEN_CAVEATS = [
    "the open is the vendor's (TickerHistory3 open), not an auction print; the research projection never carried it",
    "a factor step dated t-1 (dividend, split) is inside the overnight part, as the vendor dates it", LINE_NOTE]
_spec("ret_overnight", "price_open", "simple return, decimal: adjusted open over the previous adjusted close, minus 1",
      "row t: a = session t-1, b = session t-2 (extended axis); O_a F_a / (C_b F_b) - 1, C = vendor close, O = vendor "
      "open, F = chained factor (SOURCE_RULE); needs observations at a and b, a finite positive open at a, no kept_gap "
      "step in (b, a], and the house guard on the adjusted log ratio (|x| <= 1.5 and |x| <= |log(O_a / C_b)| + 0.10)",
      "no fill: an absent observation, open or guard failure -> NaN", _OPEN_COLS, _OPEN_CAVEATS, "vendor_open",
      "price-ret-overnight-lag1-v1", "2 extended sessions (row 0 reads the two sessions before the role)")
_spec("ret_intraday", "price_open", "simple return, decimal: close over open of the same session, minus 1",
      "row t: a = session t-1; C_a / O_a - 1 (the factor cancels within a session); needs an observation at a with a "
      "finite positive open, and |log(C_a / O_a)| <= 1.5",
      "no fill: an absent observation or open, or a guard failure -> NaN", _OPEN_COLS, _OPEN_CAVEATS, "vendor_open",
      "price-ret-intraday-lag1-v1", "1 extended session")
_spec("ceq_iss_5y", "price_ceq",
      "log ratio: 5-year composite equity issuance (Daniel-Titman 2006), ln(ME_a / ME_b) - ln(P_a / P_b)",
      "row t: a = session t-1, b = a - 1260 sessions (extended axis). ME_s = 1000 x shares(o_s) x F_s / F(o_s) x C_s: "
      "the A8 house market equity, o_s = the line's last share observation dated <= date(s) - 90 days and >= date(s) - "
      "490 days (vendor shares in thousands, 0 < shares <= the A9 ceiling). The price terms cancel exactly, so the "
      "value is ln(q(o_a) / q(o_b)), q = shares / F: share growth net of splits (they move shares and F together) and "
      "of dividends (a dividend raises F: paid-out cash counts as negative issuance, as in Daniel-Titman). Needs price "
      "observations at a and b, both share observations, no kept_gap step in (o_b, a], the line not withheld under "
      "C-81 at a (its first above-ceiling row dated <= date(a)), and a value inside the declared domain",
      "no fill: any missing term -> NaN; outside [-ln 100, ln 100] -> NaN, counted (outside_domain_member_cells)",
      ["shares", "cumulReturnFactor", "close", "volume", "tradingDate", "securityID"],
      ["vendor share runs start at the filing cover date; the 90-day lag (A8) keeps each share count point in time",
       "one price line's share count (an ADR line counts ADS), not the issuer total across share classes",
       "genuine splits missing from the vendor factor are not restated (as shares_out)"],
      "vendor_shares", "price-ceq-iss-5y-lag1-v1",
      "1,261 extended sessions plus the 490-day share lookback before the role start", domain=CEQ_DOMAIN)
_spec("coskew_60m", "price_coskew",
      "dimensionless: standardised coskewness E[e_i e_m^2] / (sqrt(E[e_i^2]) E[e_m^2]) (Harvey-Siddique 2000)",
      "row t: month ends e_k = (t-1) - 21k, k = 0..60 (extended axis); r_k = P(e_k) / P(e_(k+1)) - 1 per line (needs "
      "observations at both ends, no kept_gap step inside, |log adj ratio| <= |log raw ratio| + 0.10); m_k = the "
      "monthly vendor EW market (MARKET_RULE); K = months with both finite; |K| >= 48, else NaN. On K: OLS r = alpha + "
      "beta m, "
      "e_i = the residual, e_m = m - mean(m); value = mean(e_i e_m^2) / (sqrt(mean(e_i^2)) mean(e_m^2)); NaN when "
      "either variance is 0. Raw returns (no risk-free rate: a constant shift leaves e_i and e_m unchanged)",
      "no fill: fewer than 48 usable months -> NaN",
      ["close", "cumulReturnFactor", "volume", "tradingDate", "securityID"],
      ["the market is every vendor line (ETFs, ADRs and preferred lines included), equal weighted and rebalanced daily",
       "monthly means 21 sessions, anchored at t-1 and moving daily, not calendar months", LINE_NOTE],
      "vendor_prices", "price-coskew-60m-lag1-v1", "1,261 extended sessions before the role start")
_spec("vol_126", "price_volume", "shares per session: mean raw daily share volume",
      "row t >= 126: mean of the role's volume.f64 over the sessions s in t-126..t-1 with present[s] == 1 and a finite "
      "volume >= 0; NaN when fewer than 63 such sessions or t < 126. Raw share volume in each session's own share "
      "units, as the DSL's volume (a split inside the window mixes share bases)",
      "no fill: fewer than 63 present sessions in the window -> NaN", ["volume.f64", "present.u8"],
      ["the role's volume.f64 is raw share volume (volume_basis raw-share-volume), not restated across splits",
       LINE_NOTE],
      "role", "price-vol-126-lag1-v1", "126 role sessions", clock=ROLE_CLOCK)
_spec("xrd0_ttm", "price_rd", "USD: research and development expense, trailing twelve months, missing -> 0",
      "row t: xrd_ttm[t] when finite; else 0 when sale_ttm[t] is finite (a visible, fresh filing that reports no R&D); "
      "else NaN. Both are this run's issuer fields (same link, clock, lag and staleness)",
      "inherits xrd_ttm / sale_ttm: NaN when neither is finite", ["xrd_ttm", "sale_ttm"],
      ["a filer that reports R&D inside another expense line reads 0, as a filer without R&D"],
      "fields", "price-xrd0-ttm-v1", "the fundamentals lag", requires=["xrd_ttm", "sale_ttm"], clock=RD_CLOCK)

OPEN_NAMES = ("ret_overnight", "ret_intraday")
# The producing code of each field group (contract of task C-3: {group: (entry functions,)}); the module-level
# definitions they reach are part of each producer. compute() orchestrates and is never a producer.
PRODUCERS = {"price_open": ("source_panel", "open_return_rows"),
             "price_ceq": ("source_panel", "ceq_iss_rows"),
             "price_coskew": ("source_panel", "vendor_market", "coskew_rows"),
             "price_volume": ("volume_mean_rows",),
             "price_rd": ("zero_filled_rows",)}


# ---------------------------------------------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------------------------------------------

def require_open(path: Path, names) -> None:
    """``FieldNeedsOpen`` unless the price source's schema has a float ``open`` column (a footer read only)."""
    schema = pq.ParquetFile(Path(path), memory_map=False).schema_arrow
    i = schema.get_field_index("open")
    if i < 0 or schema.field(i).type not in OPEN_TYPES:
        raise FieldNeedsOpen(f"{', '.join(names)}: field needs open; absent in export ({Path(path)} has no float "
                             "'open' column; the research projection prepare_recent_research.COLUMNS has none either)")


def extended_days(role, pre_sessions: int, lookback_days: int = 0):
    """(epoch days, prefix): ``pre_sessions`` NYSE rule sessions before the role start (and every rule session in the
    ``lookback_days`` before the first of them), then the role's own sessions. Role row t is extended index
    t + prefix."""
    epoch = dt.date(1970, 1, 1)
    first = int(role.days[0])
    before = lambda lo: nyse_sessions(epoch + dt.timedelta(days=lo), epoch + dt.timedelta(days=first - 1))  # noqa: E731
    rule = before(first - 2 * pre_sessions - 14)
    if len(rule) < pre_sessions:
        raise ValueError("internal: the NYSE rule calendar is shorter than the requested history")
    if pre_sessions:
        rule = rule[len(rule) - pre_sessions:]
        if lookback_days:
            rule = before(int(rule[0]) - lookback_days)
    else:
        rule = rule[:0]
    return np.concatenate((rule.astype(np.int64), role.days.astype(np.int64))), len(rule)


class Gaps:
    """Kept_gap steps of factor-break-v1: ``crosses(lo, hi)`` is true where a line's span of extended rows (lo, hi]
    contains one (then the chained factor cannot bridge it)."""

    def __init__(self, days: np.ndarray, fb: dict, n: int, h):
        keep = fb["action"] == h.FB_KEPT_GAP
        j, t_day = fb["j"][keep], fb["t_day"][keep]
        self.lines = np.unique(j)
        self.col = np.full(n, -1, dtype=np.int64)
        self.col[self.lines] = np.arange(len(self.lines))
        self.epoch = np.zeros((len(days), len(self.lines)), dtype=np.int32)
        if len(self.lines):
            np.add.at(self.epoch, (np.searchsorted(days, t_day), self.col[j]), 1)
            np.cumsum(self.epoch, axis=0, out=self.epoch)
        self.count = int(len(j))

    def crosses(self, lo, hi, shape) -> np.ndarray:
        """``shape`` (..., lines) booleans; ``lo`` / ``hi`` are extended rows broadcast to it."""
        out = np.zeros(shape, dtype=bool)
        if len(self.lines):
            cols, g = self.lines, self.col[self.lines]
            lo_c = np.broadcast_to(np.asarray(lo, dtype=np.int64), shape)[..., cols]
            hi_c = np.broadcast_to(np.asarray(hi, dtype=np.int64), shape)[..., cols]
            out[..., cols] = self.epoch[hi_c, g] != self.epoch[lo_c, g]
        return out


def verify_source(h, path: Path, role, budget):
    """Hash the price source once: it must be the file the role was projected from. Returns (identity, SHA-256)."""
    path = Path(path)
    captured = h.identity(path)
    budget.report("price-source-hash-start", bytes=captured[2])
    digest = h.sha_file(path, budget)
    if role.source_sha256 is None or digest != role.source_sha256:
        raise ValueError("--price-source SHA-256 differs from the role's source_sha256 (not the file the role was "
                         "projected from)")
    if h.identity(path) != captured:
        raise ValueError("--price-source changed during hashing")
    return captured, digest


def source_panel(h, path: Path, verified, role, budget, *, pre_sessions: int, lookback_days: int, shares: bool,
                 open_: bool) -> dict:
    """The role lines' vendor observations on the extended axis (``SOURCE_RULE``): chained factor ``F``, raw close
    ``C`` (f32), and optionally vendor shares (thousands, f32; A9 rows NaN) and open (f32). ``verified`` is
    ``verify_source``'s result; the file must be unchanged since."""
    path = Path(path)
    captured, digest = verified
    pf = pq.ParquetFile(path, memory_map=False)
    columns = ["tradingDate", "securityID", "close", "volume", "cumulReturnFactor"] + (["shares"] if shares else [])
    schema = pf.schema_arrow
    for c in columns:
        i = schema.get_field_index(c)
        if i < 0 or schema.field(i).type != h.TH_TYPES[c]:
            raise ValueError(f"--price-source column {c} is missing or not {h.TH_TYPES[c]}")
    if open_:
        require_open(path, OPEN_NAMES)
        columns.append("open")
    days, prefix = extended_days(role, pre_sessions, lookback_days)
    n_ext, n = len(days), role.n
    seal_day = h.day_of(h.SEAL)
    first, last = int(days[0]), min(int(role.days[-1]), seal_day - 1)
    budget.admit(n_ext * n * (8 + 4 + 2 + (4 if shares else 0) + (4 if open_ else 0)) + (64 << 20),
                 "price-source-matrices")
    counts = np.zeros((n_ext, n), dtype=np.uint16)
    crf = np.full((n_ext, n), np.nan)
    raw = np.full((n_ext, n), np.nan, dtype=np.float32)
    shr = np.full((n_ext, n), np.nan, dtype=np.float32) if shares else None
    opn = np.full((n_ext, n), np.nan, dtype=np.float32) if open_ else None
    first_above = np.full(n, NEVER, dtype=np.int64)
    st = {"rows_scanned": 0, "rows_on_or_after_seal_skipped": 0, "rows_selected": 0, "rows_off_calendar": 0,
          "shares_rows_above_a9_ceiling": 0}
    for b, batch in enumerate(pf.iter_batches(batch_size=65536, columns=columns, use_threads=False)):
        budget.check("price-source-batch")
        st["rows_scanned"] += batch.num_rows
        d = pc.fill_null(batch.column("tradingDate").cast(pa.int32()), -1).to_numpy().astype(np.int64)
        st["rows_on_or_after_seal_skipped"] += int(np.count_nonzero(d >= seal_day))
        idx = np.flatnonzero((d >= first) & (d <= last))
        if not len(idx):
            continue
        sid = pc.fill_null(batch.column("securityID"), 0).to_numpy()[idx]
        pos, on = role.columns_of(sid)
        idx, j, dd = idx[on], pos[on], d[idx][on]
        e = np.minimum(np.searchsorted(days, dd), n_ext - 1)
        cal = days[e] == dd
        st["rows_off_calendar"] += int(np.count_nonzero(~cal))
        idx, j, e = idx[cal], j[cal], e[cal]
        if not len(idx):
            continue
        st["rows_selected"] += len(idx)
        np.add.at(counts, (e, j), 1)
        sub = batch.take(pa.array(idx, type=pa.int64()))
        factor = sub.column("cumulReturnFactor").to_numpy(zero_copy_only=False).astype(np.float64)
        close = sub.column("close").to_numpy(zero_copy_only=False).astype(np.float32)
        volume = sub.column("volume").to_numpy(zero_copy_only=False).astype(np.float64)
        with np.errstate(invalid="ignore"):
            obs = (np.isfinite(factor) & (factor > 0) & np.isfinite(close) & (close > 0) & np.isfinite(volume)
                   & (volume >= 0))
        crf[e, j] = np.where(obs, factor, np.nan)
        raw[e, j] = np.where(obs, close, np.float32(np.nan))
        if shares:
            s = pc.fill_null(sub.column("shares"), 0).to_numpy().astype(np.float64)
            above = s > h.THOUSANDS_ROW_CEILING
            st["shares_rows_above_a9_ceiling"] += int(np.count_nonzero(above))
            np.minimum.at(first_above, j[above], days[e[above]])
            shr[e, j] = np.where(obs & (s > 0) & ~above, s, np.nan).astype(np.float32)
        if open_:
            o = sub.column("open").to_numpy(zero_copy_only=False).astype(np.float64)
            with np.errstate(invalid="ignore"):
                opn[e, j] = np.where(obs & np.isfinite(o) & (o > 0), o, np.nan).astype(np.float32)
    if h.identity(path) != captured:
        raise ValueError("--price-source changed while reading")
    dup = counts > 1
    st["duplicate_keys_quarantined"] = int(np.count_nonzero(dup))
    for m in (crf, raw, shr, opn):
        if m is not None:
            m[dup] = np.nan
    del counts, dup
    fb = h.factor_breaks(crf, raw, days, budget)
    rep = fb["action"] == h.FB_REPAIRED
    for j, t_day, k in zip(fb["j"][rep], fb["t_day"][rep], fb["k"][rep]):
        crf[int(np.searchsorted(days, t_day)):, int(j)] /= k   # F chained: the repaired step divided out
    gaps = Gaps(days, fb, n, h)
    st.update({"extended_sessions": n_ext, "sessions_before_role": prefix, "first_session": h.date_of(days[0]),
               "calendar": "nyse-rule-v1 before the role, the role's sessions inside it",
               "factor_break": {"rule": h.FB_RULE, "mass_sessions": [h.date_of(days[x]) for x in fb["mass"]],
                                "repaired_steps": int(np.count_nonzero(rep)), "kept_gap_steps": gaps.count},
               "shares_lines_withheld_c81": int(np.count_nonzero(first_above != NEVER)) if shares else None,
               "rule": SOURCE_RULE})
    source = {"path": str(path.resolve()), "bytes": captured[2], "sha256": digest,
              "row_groups": pf.metadata.num_row_groups, "rows": pf.metadata.num_rows}
    return {"days": days, "prefix": prefix, "F": crf, "C": raw, "shares": shr, "open": opn,
            "first_above": first_above, "gaps": gaps, "stats": st, "source": source}


def vendor_market(h, path: Path, verified, days: np.ndarray, lo: int, budget, spill: Path):
    """Daily vendor EW market (``MARKET_RULE``) on the extended sessions days[lo:]: (mu over every extended session,
    NaN before lo, stats). One DuckDB query on one thread (deterministic), spilling into ``spill`` (removed).
    ``verified`` is ``verify_source``'s result; the file must be unchanged after the query."""
    import duckdb
    cal_days = days[lo:]
    cal = pa.table({"d": pa.array(cal_days.astype("datetime64[D]")),
                    "prev": pa.array(np.concatenate(([cal_days[0] - 1], cal_days[:-1])).astype("datetime64[D]"))})
    literal = "'" + Path(path).as_posix().replace("'", "''") + "'"
    seal = h.SEAL.isoformat()
    sql = f"""
    WITH src AS (
      SELECT tradingDate AS d, securityID AS id, CAST(close AS DOUBLE) AS raw, cumulReturnFactor AS f, volume AS v,
             count(*) OVER (PARTITION BY tradingDate, securityID) AS copies
      FROM read_parquet({literal})
      WHERE tradingDate >= DATE '{h.date_of(cal_days[0])}' AND tradingDate <= DATE '{h.date_of(cal_days[-1])}'
        AND tradingDate < DATE '{seal}'),
    obs AS (
      SELECT s.d, s.id, s.raw, s.f FROM src s JOIN cal c ON s.d = c.d
      WHERE s.id > 0 AND s.copies = 1 AND isfinite(s.raw) AND s.raw > 0 AND isfinite(s.f) AND s.f > 0
        AND isfinite(s.v) AND s.v >= 0),
    steps AS (
      SELECT d, lag(d) OVER w AS d0, ln(raw) - ln(lag(raw) OVER w) AS r, ln(f) - ln(lag(f) OVER w) AS s
      FROM obs WINDOW w AS (PARTITION BY id ORDER BY d)),
    cells AS (
      SELECT st.d, st.r, st.s, st.r + st.s AS a FROM steps st JOIN cal c ON st.d = c.d AND st.d0 = c.prev),
    flagged AS (
      SELECT d, a, (abs(s) > {h.FB_CELL_STEP!r} AND abs(a) > abs(r) + {h.FB_CELL_EXCESS!r}) AS jump,
             (abs(a) <= {GUARD_LOG!r} AND abs(a) <= abs(r) + {GUARD_EXCESS!r}) AS kept
      FROM cells)
    SELECT d, count(*) FILTER (WHERE kept AND NOT jump) AS n,
           fsum(exp(a) - 1) FILTER (WHERE kept AND NOT jump) AS total,
           count(*) FILTER (WHERE jump) AS jumps, count(*) FILTER (WHERE NOT kept) AS guarded
    FROM flagged GROUP BY d ORDER BY d"""
    spill.mkdir(parents=False, exist_ok=False)
    limit_mib = max(128, (budget.max_rss - budget.rss()) >> 21)
    con = duckdb.connect()
    try:
        con.execute("SET threads=1")
        con.execute(f"SET memory_limit='{limit_mib}MiB'")
        con.execute("SET preserve_insertion_order=false")
        con.execute("SET temp_directory=" + "'" + spill.as_posix().replace("'", "''") + "'")
        con.register("cal", cal)
        budget.check("price-market-start")
        res = con.execute(sql)
        out = (getattr(res, "to_arrow_table", None) or res.fetch_arrow_table)()   # duckdb >= 1.4 / older
    finally:
        con.close()
        shutil.rmtree(spill, ignore_errors=True)
    if h.identity(Path(path)) != verified[0]:
        raise ValueError("--price-source changed while the market was read")
    budget.check("price-market")
    d = out.column("d").cast(pa.int32()).to_numpy().astype(np.int64)
    n = out.column("n").to_numpy(zero_copy_only=False).astype(np.int64)
    total = out.column("total").to_numpy(zero_copy_only=False).astype(np.float64)
    at = np.searchsorted(days, d)
    if len(d) and (np.any(at >= len(days)) or np.any(days[np.minimum(at, len(days) - 1)] != d)):
        raise ValueError("internal: market sessions off the extended calendar")
    mu = np.full(len(days), np.nan)
    with np.errstate(invalid="ignore", divide="ignore"):
        mu[at] = np.where(n > 0, total / np.maximum(n, 1), np.nan)
    inside = mu[lo + 1:]
    k = n[n > 0]
    stats = {"rule": MARKET_RULE, "first_session": h.date_of(cal_days[0]), "sessions": int(len(cal_days) - 1),
             "sessions_without_contributors": int(np.count_nonzero(~np.isfinite(inside))),
             "contributors_min": int(k.min()) if len(k) else None,
             "contributors_median": float(np.median(k)) if len(k) else None,
             "contributors_max": int(k.max()) if len(k) else None,
             "jump_cells_excluded": int(out.column("jumps").to_numpy(zero_copy_only=False).sum()) if len(d) else 0,
             "guarded_cells_excluded": int(out.column("guarded").to_numpy(zero_copy_only=False).sum()) if len(d) else 0}
    return mu, stats


# ---------------------------------------------------------------------------------------------------------------
# Producers: each returns {name: (writer, sources, extras)}
# ---------------------------------------------------------------------------------------------------------------

class _Writers:
    """The requested fields' writers; on a refusal every handle is closed (partial files stay unpublished)."""

    def __init__(self, h, output: Path, names, role):
        self.w = {}
        try:
            for x in names:
                self.w[x] = h.FieldWriter(output, x, role)
        except BaseException:
            self.abort()
            raise

    def abort(self):
        for w in self.w.values():
            w.f.close()

    def close(self):
        for w in self.w.values():
            w.close()


def open_return_rows(h, panel: dict, role, output: Path, budget, names) -> dict:
    """``ret_overnight`` / ``ret_intraday`` (their field definitions)."""
    n, pre = role.n, panel["prefix"]
    F, C, O, gaps = panel["F"], panel["C"], panel["open"], panel["gaps"]
    ws = _Writers(h, output, names, role)
    guarded = dict.fromkeys(names, 0)
    try:
        for t in range(role.n_dates):
            a = pre + t - 1
            member = role.member[t] != 0
            o, ca, fa = O[a].astype(np.float64), C[a].astype(np.float64), F[a]
            with np.errstate(invalid="ignore", divide="ignore"):
                if "ret_overnight" in ws.w:
                    cb, fb_ = C[a - 1].astype(np.float64), F[a - 1]
                    ok = np.isfinite(o) & np.isfinite(fa) & np.isfinite(cb) & np.isfinite(fb_)
                    ok &= ~gaps.crosses(a - 1, a, (n,))
                    adj = np.log(o * fa) - np.log(cb * fb_)
                    rw = np.log(o) - np.log(cb)
                    bad = ok & ~((np.abs(adj) <= GUARD_LOG) & (np.abs(adj) <= np.abs(rw) + GUARD_EXCESS))
                    guarded["ret_overnight"] += int(np.count_nonzero(bad & member))
                    ws.w["ret_overnight"].write(np.where(ok & ~bad, (o * fa) / (cb * fb_) - 1.0, np.nan))
                if "ret_intraday" in ws.w:
                    ok = np.isfinite(o) & np.isfinite(ca) & np.isfinite(fa)
                    lr = np.log(ca) - np.log(o)
                    bad = ok & ~(np.abs(lr) <= GUARD_LOG)
                    guarded["ret_intraday"] += int(np.count_nonzero(bad & member))
                    ws.w["ret_intraday"].write(np.where(ok & ~bad, ca / o - 1.0, np.nan))
            if t % 256 == 0:
                budget.check("price-open-write")
    except BaseException:
        ws.abort()
        raise
    ws.close()
    return {x: (ws.w[x], [panel["source"]], {"guarded_member_cells": guarded[x]}) for x in names}


def ceq_iss_rows(h, panel: dict, role, output: Path, budget) -> dict:
    """``ceq_iss_5y`` (its field definition)."""
    n, pre, days = role.n, panel["prefix"], panel["days"]
    F, C, S, gaps = panel["F"], panel["C"], panel["shares"], panel["gaps"]
    lag, oldest = h.SHARES_LAG_DAYS, h.SHARES_LAG_DAYS + h.SHARES_MAX_AGE_DAYS
    budget.admit(len(days) * n * 4 + (16 << 20), "price-ceq-share-index")
    last_obs = np.where(np.isfinite(S), np.arange(len(days), dtype=np.int32)[:, None], np.int32(-1))
    np.maximum.accumulate(last_obs, axis=0, out=last_obs)   # last share observation row at or before each row
    cols = np.arange(n)
    lo, hi = CEQ_DOMAIN
    outside = 0
    ws = _Writers(h, output, ["ceq_iss_5y"], role)
    w = ws.w["ceq_iss_5y"]

    def lag_obs(s: int):
        c = int(np.searchsorted(days, int(days[s]) - lag, side="right")) - 1
        o = last_obs[c] if c >= 0 else np.full(n, -1, dtype=np.int32)
        ok = (o >= 0) & (days[np.maximum(o, 0)] >= int(days[s]) - oldest)
        return np.maximum(o, 0).astype(np.int64), ok

    try:
        for t in range(role.n_dates):
            a = pre + t - 1
            b = a - CEQ_SESSIONS
            row = np.full(n, np.nan)
            if b >= 0:
                oa, ok_a = lag_obs(a)
                ob, ok_b = lag_obs(b)
                ok = ok_a & ok_b & np.isfinite(C[a]) & np.isfinite(F[a]) & np.isfinite(C[b]) & np.isfinite(F[b])
                ok &= panel["first_above"] > int(days[a])
                ok &= ~gaps.crosses(ob, a, (n,))
                with np.errstate(invalid="ignore", divide="ignore"):
                    v = (np.log(S[oa, cols].astype(np.float64)) - np.log(F[oa, cols])
                         - np.log(S[ob, cols].astype(np.float64)) + np.log(F[ob, cols]))
                ok &= np.isfinite(v)
                inside = (v >= lo) & (v <= hi)
                outside += int(np.count_nonzero(ok & ~inside & (role.member[t] != 0)))
                row = np.where(ok & inside, v, np.nan)
            w.write(row)
            if t % 256 == 0:
                budget.check("price-ceq-write")
    except BaseException:
        ws.abort()
        raise
    ws.close()
    return {"ceq_iss_5y": (w, [panel["source"]], {"domain": list(CEQ_DOMAIN), "outside_domain_member_cells": outside})}


def coskew_rows(h, panel: dict, mu: np.ndarray, role, output: Path, budget) -> dict:
    """``coskew_60m`` (its field definition) from the panel and the daily vendor market ``mu``."""
    n, pre = role.n, panel["prefix"]
    F, C, gaps = panel["F"], panel["C"], panel["gaps"]
    fin = np.isfinite(mu)
    cum_log = np.cumsum(np.where(fin, np.log1p(np.where(fin, mu, 0.0)), 0.0))
    cum_nan = np.cumsum(~fin)
    steps = MONTH_SESSIONS * np.arange(COSKEW_MONTHS + 1)
    ws = _Writers(h, output, ["coskew_60m"], role)
    w = ws.w["coskew_60m"]
    short = 0
    try:
        for t in range(role.n_dates):
            e = pre + t - 1 - steps          # e[0] = t-1 ... e[60]
            row = np.full(n, np.nan)
            if e[-1] >= 0:
                end, start = e[:-1], e[1:]
                with np.errstate(invalid="ignore", divide="ignore"):
                    m = np.where(cum_nan[end] == cum_nan[start], np.expm1(cum_log[end] - cum_log[start]), np.nan)
                    p_end, p_start = C[end].astype(np.float64) * F[end], C[start].astype(np.float64) * F[start]
                    ratio = p_end / p_start
                    la = np.log(ratio)
                    lr = np.log(C[end].astype(np.float64)) - np.log(C[start].astype(np.float64))
                    ok = np.isfinite(la) & (np.abs(la) <= np.abs(lr) + GUARD_EXCESS) & np.isfinite(m)[:, None]
                ok &= ~gaps.crosses(start[:, None], end[:, None], (COSKEW_MONTHS, n))
                k = ok.sum(axis=0)
                wt = ok.astype(np.float64)
                r = np.where(ok, ratio - 1.0, 0.0)
                mm = np.where(ok, m[:, None], 0.0)
                with np.errstate(invalid="ignore", divide="ignore"):
                    kk = np.maximum(k, 1).astype(np.float64)
                    dm = (mm - mm.sum(axis=0) / kk) * wt
                    dr = (r - r.sum(axis=0) / kk) * wt
                    smm = (dm * dm).sum(axis=0)
                    beta = (dr * dm).sum(axis=0) / smm
                    eps = (dr - beta * dm) * wt
                    see = (eps * eps).sum(axis=0)
                    value = ((eps * dm * dm).sum(axis=0) / kk) / (np.sqrt(see / kk) * (smm / kk))
                good = (k >= COSKEW_MIN_MONTHS) & (smm > 0) & (see > 0) & np.isfinite(value)
                short += int(np.count_nonzero((k < COSKEW_MIN_MONTHS) & (role.member[t] != 0)))
                row = np.where(good, value, np.nan)
            w.write(row)
            if t % 128 == 0:
                budget.check("price-coskew-write")
    except BaseException:
        ws.abort()
        raise
    ws.close()
    return {"coskew_60m": (w, [panel["source"]], {"short_history_member_cells": short})}


def volume_mean_rows(h, role, output: Path, budget) -> dict:
    """``vol_126`` (its field definition), streamed through the role's verified rows."""
    n = role.n
    rows = h.RoleRows(role, ["volume.f64", "present.u8"])
    ws = _Writers(h, output, ["vol_126"], role)
    w = ws.w["vol_126"]
    ring = np.zeros((VOL_WINDOW, n))
    seen = np.zeros((VOL_WINDOW, n), dtype=bool)
    try:
        try:
            for t in range(role.n_dates):
                row = np.full(n, np.nan)
                if t >= VOL_WINDOW:
                    k = seen.sum(axis=0)
                    with np.errstate(invalid="ignore", divide="ignore"):
                        row = np.where(k >= VOL_MIN_SESSIONS, ring.sum(axis=0) / np.maximum(k, 1), np.nan)
                w.write(row)                   # row t is written before session t is read
                v, p = rows.row("volume.f64"), rows.row("present.u8") != 0
                with np.errstate(invalid="ignore"):
                    ok = p & np.isfinite(v) & (v >= 0)
                slot = t % VOL_WINDOW
                ring[slot], seen[slot] = np.where(ok, v, 0.0), ok
                if t % 256 == 0:
                    budget.check("price-vol-write")
            sources = rows.verify()
        finally:
            rows.close()
    except BaseException:
        ws.abort()
        raise
    ws.close()
    return {"vol_126": (w, sources, {})}


def zero_filled_rows(h, role, output: Path, budget) -> dict:
    """``xrd0_ttm`` (its field definition) from this run's published xrd_ttm.f64 and sale_ttm.f64."""
    size = role.n * 8
    paths = {x: output / f"{x}.f64" for x in ("xrd_ttm", "sale_ttm")}
    for x, p in paths.items():
        if not p.is_file() or p.stat().st_size != role.n_dates * size:
            raise ValueError(f"xrd0_ttm: this run's {x}.f64 is missing or has the wrong size")
    handles, hashes = {}, {x: hashlib.sha256() for x in paths}
    ws = _Writers(h, output, ["xrd0_ttm"], role)
    w = ws.w["xrd0_ttm"]
    zero = 0
    try:
        try:
            for x, p in paths.items():
                handles[x] = p.open("rb")
            for t in range(role.n_dates):
                blobs = {x: f.read(size) for x, f in handles.items()}
                for x, blob in blobs.items():
                    if len(blob) != size:
                        raise ValueError(f"xrd0_ttm: this run's {x}.f64 is truncated")
                    hashes[x].update(blob)
                xrd, sale = (np.frombuffer(blobs[x], dtype="<f8") for x in ("xrd_ttm", "sale_ttm"))
                fill = ~np.isfinite(xrd) & np.isfinite(sale)
                zero += int(np.count_nonzero(fill & (role.member[t] != 0)))
                w.write(np.where(np.isfinite(xrd), xrd, np.where(fill, 0.0, np.nan)))
                if t % 256 == 0:
                    budget.check("price-xrd0-write")
        finally:
            for f in handles.values():
                f.close()
    except BaseException:
        ws.abort()
        raise
    ws.close()
    sources = [{"path": str(p.resolve()), "bytes": role.n_dates * size, "sha256": hashes[x].hexdigest(),
                "field": x} for x, p in paths.items()]
    return {"xrd0_ttm": (w, sources, {"zero_filled_member_cells": zero})}


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "PriceFieldModule":
    return PriceFieldModule(host_namespace)


# -- --reuse interface (v8 C-3; Ruling E-21) ------------------------------------------------------------------------
def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """This run's input pins of a price field beyond the role and its required fields: none. The price source must
    hash to the role's source_sha256 and the prior is bound to the same role (load_prior)."""
    return {}


def entry_inputs(entry: dict) -> dict:
    """The same pins as a manifest entry records them (none)."""
    return {}


class PriceFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        g = parser.add_argument_group("price and long-lookback fields (research_fields_price.py: "
                                      + ",".join(FIELDS) + ")")
        g.add_argument("--price-source", type=Path,
                       help="the vendor TickerHistory3 parquet the role was projected from (SHA-256 = the role "
                            "manifest's source_sha256); needed by " + ",".join(
                                x for x, s in FIELDS.items() if s["needs"].startswith("vendor")))

    @staticmethod
    def check(selected, options: dict):
        names = [f for f in selected if f in FIELDS]
        vendor = [f for f in names if FIELDS[f]["needs"].startswith("vendor")]
        if not vendor:
            return
        if options.get("price_source") is None:
            raise ValueError(f"--fields: {', '.join(vendor)} need --price-source (the role's vendor source file)")
        needs_open = [f for f in vendor if FIELDS[f]["needs"] == "vendor_open"]
        if needs_open:
            require_open(Path(options["price_source"]), needs_open)

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        want = set(names)
        st = {"lag_sessions": LAG_SESSIONS, "clock": PRICE_CLOCK}
        results = {}
        history = bool(want & {"ceq_iss_5y", "coskew_60m"})
        if want & {"ret_overnight", "ret_intraday", "ceq_iss_5y", "coskew_60m"}:
            source = Path(options["price_source"])
            verified = verify_source(h, source, role, budget)
            pre = HISTORY_SESSIONS if history else 2
            lookback = h.SHARES_LAG_DAYS + h.SHARES_MAX_AGE_DAYS if "ceq_iss_5y" in want else 0
            mu = None
            if "coskew_60m" in want:   # before the panel: the query's memory is released before the matrices exist
                days, prefix = extended_days(role, pre, lookback)
                mu, st["market"] = vendor_market(h, source, verified, days, prefix - HISTORY_SESSIONS, budget,
                                                 output / ".price-market-spill")
            panel = source_panel(h, source, verified, role, budget, pre_sessions=pre, lookback_days=lookback,
                                 shares="ceq_iss_5y" in want, open_=bool(want & set(OPEN_NAMES)))
            st["source"] = panel["stats"]
            budget.report("price-source-loaded", rows_selected=panel["stats"]["rows_selected"])
            if want & set(OPEN_NAMES):
                results.update(open_return_rows(h, panel, role, output, budget, [x for x in names if x in OPEN_NAMES]))
            if "ceq_iss_5y" in want:
                results.update(ceq_iss_rows(h, panel, role, output, budget))
            if mu is not None:
                results.update(coskew_rows(h, panel, mu, role, output, budget))
            del panel
        if "vol_126" in want:
            results.update(volume_mean_rows(h, role, output, budget))
        if "xrd0_ttm" in want:
            results.update(zero_filled_rows(h, role, output, budget))
        source_checks[GROUP] = st
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}  # v8 C-3 --reuse
        for x in names:
            w, sources, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, LAG_SESSIONS)),
                               "lag_sessions": LAG_SESSIONS, "min_history": spec["min_history"], **extra}
            outcome[x] = (w, sources, w.coverage())
        budget.report("price-complete", fields=len(names))
