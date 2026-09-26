"""Price wave natives (tier-1 v2 node 1.12): session-calendar price and trading-friction features.

Built from the price wave inputs of :mod:`atx_db.research.spine` (the retained vendor file, never
the warehouse): one bounded pass per formation year (in line buckets) emits the month-end rows
only, and :func:`build_store` writes each native into the Parquet feature store (L2, basis
``reconstructed``) with R2b's transforms. No forward return is read here (ruling R-6).

Conventions
    * ``t`` in a window "months t-a..t-b" is the holding month (the month after the formation
      month ``F`` = the month of ``eom``): months ``t-a..t-b`` are formation months
      ``F-(a-1)..F-(b-1)``. A month's price is the repaired adjusted close of the line's last
      observed session in that month (session-based month ends), so
      ``ret_12_1 = adj(F-1) / adj(F-12) - 1``.
    * Daily windows are the last N XNYS rule sessions ending at the formation session (the
      month's last rule session). A line *observes* a session when it has a valid bar on it; a
      daily return exists only between two consecutive rule sessions both observed.
    * Minimum observations are the brief's: observed sessions for price-level and momentum
      windows, daily returns (or pairs) for return statistics, market/line pairs for betas.
      Below the minimum the row is ``insufficient_obs``; a line without a bar at the formation
      session is ``no_session_bar``; a missing input (the lagged share count, the EW market) is
      ``missing_input``.
    * EW market (P3 sealed convention, :func:`spine.stage_market`): the equal-weighted daily bar
      return of the prior formation's spine lines, each line's return winsorized at +-50% before
      the mean (ruling C-55; the unclipped mean is a diagnostic only). Returns are simple unless
      a native says log.
    * The value clock is the formation session's bar clock (22:00 UTC on the session), at or
      before the month end's decision clock.
"""

from __future__ import annotations

import os
import time
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from . import spine as _spine
from .research_lake import sha256_file, sql_text

NATIVES_VERSION = "price-wave-natives-v1"
PRODUCER = "research.price_natives"
#: ``NATIVE_FEATURES`` gate: these natives are built into the research feature store by this
#: module, never by the warehouse panel (``panel.canonical_features`` refuses them).
REQUIRES_STORE = "research_store_price_wave"
STORE_BASIS = "reconstructed"
WAVE = "w1_price"
AMIHUD_SCALE = 1e6
BAR = "equity_daily_bars."
_PRICE = (f"{BAR}close", f"{BAR}adjusted_close")
_OHLC = (*_PRICE, f"{BAR}high", f"{BAR}low")
_VOLUME = (*_PRICE, f"{BAR}volume")
_SHARES = (*_VOLUME, f"{BAR}shares_outstanding")


@dataclass(frozen=True)
class Native:
    feature_id: str
    kind: str                     # daily | month | week | point
    expression: str
    min_obs: int
    lookback_sessions: int        # the catalog's min_history_sessions: sessions a value reaches back
    inputs: tuple[str, ...] = _PRICE
    size: bool = False            # reads the vendor share count
    positive: bool = False        # domain positive_value_required (log_winsor_z)


NATIVES: tuple[Native, ...] = (
    Native("ret_12_1", "month", "adj(F-1)/adj(F-12) - 1: months t-12..t-2, session-based month ends", 200, 252),
    Native("ret_6_1", "month", "adj(F-1)/adj(F-6) - 1: months t-6..t-2", 100, 126),
    Native("ret_9_1", "month", "adj(F-1)/adj(F-9) - 1: months t-9..t-2", 150, 189),
    Native("ret_12_7", "month", "adj(F-6)/adj(F-12) - 1: months t-12..t-7", 100, 252),
    Native("ret_36_13", "month", "adj(F-12)/adj(F-36) - 1: months t-36..t-13", 400, 756),
    Native("ret_60_13", "month", "adj(F-12)/adj(F-60) - 1: months t-60..t-13", 800, 1260),
    Native("chmom", "month", "ret_6_1(F) - ret_6_1(F-6)", 200, 252),
    Native("frog_in_pan", "month",
           "sign(ret_12_1) x (share of negative - share of positive daily returns) over months t-12..t-2", 200, 252),
    Native("seas_1_1an", "month", "the month-t-12 return: adj(F-11)/adj(F-12) - 1", 15, 252),
    Native("seas_2_5an", "month", "mean of the same-calendar-month returns of years 2-5 (months F-23, F-35, F-47, "
           "F-59); >= 3 of 4 (each with >= 15 observed sessions)", 3, 1260),
    Native("ret_1_0", "month", "adj(formation session)/adj(F-1) - 1: the formation month's return", 15, 22),
    Native("rvol_21d", "daily", "sample stdev of daily log returns, last 21 sessions", 15, 22, positive=True),
    Native("rvol_252d", "daily", "sample stdev of daily log returns, last 252 sessions", 200, 253, positive=True),
    Native("rmax5_21d", "daily", "mean of the 5 largest daily returns, last 21 sessions", 15, 22),
    Native("rmax1_21d", "daily", "largest daily return, last 21 sessions", 15, 22),
    Native("rskew_252d", "daily", "sample skewness of daily returns, last 252 sessions", 200, 253),
    Native("beta_ew_252d", "daily", "OLS slope of daily returns on the EW market (P3 convention), last 252 "
           "sessions", 200, 253),
    Native("ivol_ew_252d", "daily", "residual stdev of the 252-session CAPM regression on the EW market", 200, 253,
           positive=True),
    Native("ivol_ew_21d", "daily", "residual stdev of the 21-session CAPM regression on the EW market", 15, 22,
           positive=True),
    Native("beta_dimson_252d", "daily", "sum of the slopes of daily returns on the EW market at t-1, t and t+1 "
           "(one regression; pairs with t+1 after the formation session are dropped), last 252 sessions",
           200, 253),
    Native("beta_down_252d", "daily", "OLS slope on the EW market over the days it fell, last 252 sessions",
           100, 253),
    Native("coskew_252d", "daily", "Harvey-Siddique coskewness: E[e_i e_m^2] / (sqrt(E[e_i^2]) E[e_m^2]) with e_i "
           "the CAPM residual and e_m the demeaned EW market, last 252 sessions", 200, 253),
    Native("beta_bab_1260d", "month", "Frazzini-Pedersen: correlation of overlapping 3-session log returns with "
           "the EW market over the 60 months to F (>= 750) x sd(line) / sd(market) of daily log returns over the "
           "last 252 sessions (>= 120 line returns)", 750, 1260),
    Native("zero_trade_21d", "daily", "share of observed sessions with zero volume, last 21 sessions", 15, 21,
           inputs=_VOLUME),
    Native("zero_trade_252d", "daily", "share of observed sessions with zero volume, last 252 sessions", 200, 252,
           inputs=_VOLUME),
    Native("turnover_126d", "daily", "mean daily volume (restated to the lag date's share basis) / shares_lagged, "
           "last 126 sessions", 100, 126, inputs=_SHARES, size=True, positive=False),
    Native("turnover_252d", "daily", "mean daily volume (restated to the lag date's share basis) / shares_lagged, "
           "last 252 sessions", 200, 252, inputs=_SHARES, size=True),
    Native("std_turn_126d", "daily", "stdev of daily turnover (volume restated / shares_lagged), last 126 sessions",
           100, 126, inputs=_SHARES, size=True),
    Native("std_dvol_126d", "daily", "stdev of daily dollar volume (close x volume), last 126 sessions", 100, 126,
           inputs=_VOLUME, positive=True),
    Native("ami_126d", "daily", "1e6 x mean(|r| / (close x volume)) over positive-volume daily returns, last 126 "
           "sessions", 100, 127, inputs=_VOLUME),
    Native("ami_252d", "daily", "1e6 x mean(|r| / (close x volume)) over positive-volume daily returns, last 252 "
           "sessions", 200, 253, inputs=_VOLUME),
    Native("bidask_cs_21d", "daily", "Corwin-Schultz high-low spread (overnight-adjusted two-session estimates, "
           "negatives set to 0, averaged), last 21 sessions", 15, 22, inputs=_OHLC),
    Native("bidask_ar_21d", "daily", "Abdi-Ranaldo close-high-low spread: sqrt(max(4 mean[(c_t - eta_t)"
           "(c_t - eta_t+1)], 0)), last 21 sessions", 15, 22, inputs=_OHLC),
    Native("prc_log", "point", "close at the formation session (log by the transform)", 1, 1, positive=True),
    Native("prc_highprc_252d", "daily", "adj(formation session) / max adj over the last 252 sessions", 200, 252),
    Native("me_line_log", "point", "price x shares_lagged (the spine's me_line; log by the transform)", 1, 63,
           inputs=_SHARES, size=True, positive=True),
    Native("dolvol_126d", "daily", "mean daily dollar volume (close x volume), last 126 sessions (log by the "
           "transform)", 100, 126, inputs=_VOLUME, positive=True),
    Native("price_delay_52w", "week", "Hou-Moskowitz D1 = 1 - R2(restricted) / R2(unrestricted) of weekly returns on "
           "the weekly EW market and its 4 lags, 52 complete weeks to the formation session", 40, 280),
)
NATIVE_BY_ID: Mapping[str, Native] = {native.feature_id: native for native in NATIVES}


def panel_native_specs() -> dict[str, dict[str, Any]]:
    """The ``research.panel.NATIVE_FEATURES`` registration of every price wave native (catalog validation)."""
    return {native.feature_id: {
        "metric_window": "daily", "expression": native.expression, "inputs": list(native.inputs),
        "version": "1", "producer": PRODUCER, "requires": REQUIRES_STORE, "scope": "price_line",
        "size": native.size, "min_history_sessions": native.lookback_sessions, "min_observed": native.min_obs,
        "source": "retained TickerHistory3.parquet (the equity_daily_bars vendor feed) via research.spine",
        "adjustment_repair": "vendor_artifact_repair_v2"} for native in NATIVES}


def natives_code_digest() -> str:
    """This module plus the spine's shared helpers, constants, VA1 repair and calendar."""
    return _spine.stage_digest(files=[Path(__file__)])


# ---------------------------------------------------------------------------
# The per-year pass
# ---------------------------------------------------------------------------

def _daily_sql(fsno_lo: int, fsno_hi: int) -> str:
    """Per (eom, line) window aggregates over the last 252 sessions (``g`` = sessions before formation)."""
    both = "r IS NOT NULL AND m IS NOT NULL"
    dim = "g >= 1 AND r IS NOT NULL AND m IS NOT NULL AND m_prev IS NOT NULL AND m_next IS NOT NULL"
    turn = "volume * fac_lag / fac"
    ami = "g < {n} AND r IS NOT NULL AND volume > 0 AND close > 0"
    parts = [
        "count(*) FILTER (WHERE g < 21) AS n_obs_21",
        "count(lr) FILTER (WHERE g < 21) AS n_lr_21", "stddev_samp(lr) FILTER (WHERE g < 21) AS rvol_21",
        "count(r) FILTER (WHERE g < 21) AS n_r_21", "max(r) FILTER (WHERE g < 21) AS rmax1_21",
        "list_avg(max(r, 5) FILTER (WHERE g < 21)) AS rmax5_21",
        "count(volume) FILTER (WHERE g < 21) AS n_vol_21",
        "count(*) FILTER (WHERE g < 21 AND volume = 0) AS n_zero_21",
        f"count(*) FILTER (WHERE g < 21 AND {both}) AS c21_n",
        f"sum(m) FILTER (WHERE g < 21 AND {both}) AS c21_sx", f"sum(r) FILTER (WHERE g < 21 AND {both}) AS c21_sy",
        f"sum(m * m) FILTER (WHERE g < 21 AND {both}) AS c21_sxx",
        f"sum(r * r) FILTER (WHERE g < 21 AND {both}) AS c21_syy",
        f"sum(r * m) FILTER (WHERE g < 21 AND {both}) AS c21_sxy",
        "count(cs) FILTER (WHERE g < 21) AS n_cs_21", "avg(cs) FILTER (WHERE g < 21) AS cs_21",
        "count(ar2) FILTER (WHERE g < 21) AS n_ar_21", "avg(ar2) FILTER (WHERE g < 21) AS ar_21",
        "count(volume) FILTER (WHERE g < 126) AS n_vol_126",
        f"avg({turn}) FILTER (WHERE g < 126) AS turn_126", f"stddev_samp({turn}) FILTER (WHERE g < 126) AS sdturn_126",
        "avg(close * volume) FILTER (WHERE g < 126) AS dvol_126",
        "stddev_samp(close * volume) FILTER (WHERE g < 126) AS sddvol_126",
        f"count(*) FILTER (WHERE {ami.format(n=126)}) AS n_ami_126",
        f"avg(abs(r) / (close * volume)) FILTER (WHERE {ami.format(n=126)}) AS ami_126",
        "count(*) AS n_obs_252", "count(lr) AS n_lr_252", "stddev_samp(lr) AS rvol_252",
        "count(r) AS n_r_252", "skewness(r) AS rskew_252", "max(adj) AS high_252",
        "count(volume) AS n_vol_252", "count(*) FILTER (WHERE volume = 0) AS n_zero_252",
        f"avg({turn}) AS turn_252",
        f"count(*) FILTER (WHERE {ami.format(n=252)}) AS n_ami_252",
        f"avg(abs(r) / (close * volume)) FILTER (WHERE {ami.format(n=252)}) AS ami_252",
        f"count(*) FILTER (WHERE {both}) AS c_n", f"sum(m) FILTER (WHERE {both}) AS c_sx",
        f"sum(r) FILTER (WHERE {both}) AS c_sy", f"sum(m * m) FILTER (WHERE {both}) AS c_sxx",
        f"sum(r * r) FILTER (WHERE {both}) AS c_syy", f"sum(r * m) FILTER (WHERE {both}) AS c_sxy",
        f"sum(m * m * m) FILTER (WHERE {both}) AS c_sxxx", f"sum(r * m * m) FILTER (WHERE {both}) AS c_sxxy",
        f"count(*) FILTER (WHERE {both} AND m < 0) AS d_n", f"sum(m) FILTER (WHERE {both} AND m < 0) AS d_sx",
        f"sum(r) FILTER (WHERE {both} AND m < 0) AS d_sy", f"sum(m * m) FILTER (WHERE {both} AND m < 0) AS d_sxx",
        f"sum(r * m) FILTER (WHERE {both} AND m < 0) AS d_sxy",
        f"count(*) FILTER (WHERE {dim}) AS k_n", f"sum(r) FILTER (WHERE {dim}) AS k_y",
        f"sum(r * r) FILTER (WHERE {dim}) AS k_yy",
    ]
    names = ("m_prev", "m", "m_next")
    for i, a in enumerate(names):
        parts.append(f"sum({a}) FILTER (WHERE {dim}) AS k_x{i}")
        parts.append(f"sum(r * {a}) FILTER (WHERE {dim}) AS k_xy{i}")
        for j in range(i, 3):
            parts.append(f"sum({a} * {names[j]}) FILTER (WHERE {dim}) AS k_xx{i}{j}")
    return ",\n               ".join(parts)


_CS_K = 3.0 - 2.0 * np.sqrt(2.0)


def _pairs_sql() -> str:
    """Two-session spread terms indexed by the pair's later session (the prior one must be the previous
    rule session): Corwin-Schultz (overnight-adjusted) and Abdi-Ranaldo, on factor-adjusted prices."""
    k = repr(float(_CS_K))
    h1, l1, c1 = "(p_high * p_fac)", "(p_low * p_fac)", "(p_close * p_fac)"
    up = f"greatest(low * fac - {c1}, 0.0)"
    dn = f"greatest({c1} - high * fac, 0.0)"
    h2, l2 = f"(high * fac - {up} + {dn})", f"(low * fac - {up} + {dn})"
    beta = f"(power(ln({h1} / {l1}), 2) + power(ln({h2} / {l2}), 2))"
    gamma = f"power(ln(greatest({h1}, {h2}) / least({l1}, {l2})), 2)"
    alpha = f"((sqrt(2.0 * {beta}) - sqrt({beta})) / {k} - sqrt({gamma} / {k}))"
    ok = ("p_sno = sno - 1 AND p_high > 0 AND p_low > 0 AND high > 0 AND low > 0 AND p_close > 0 "
          "AND p_fac > 0 AND fac > 0")
    cs = f"CASE WHEN {ok} THEN greatest(2.0 * (exp({alpha}) - 1.0) / (1.0 + exp({alpha})), 0.0) END"
    ar = (f"CASE WHEN {ok} THEN 4.0 * (ln({c1}) - (ln({h1}) + ln({l1})) / 2.0) "
          f"* (ln({c1}) - (ln(high * fac) + ln(low * fac)) / 2.0) END")
    return f"{cs} AS cs, {ar} AS ar2"


_MONTH_OFFSETS = (1, 6, 7, 9, 11, 12, 23, 24, 35, 36, 47, 48, 59, 60)
_OBS_RANGES = {"1_11": (1, 11), "1_5": (1, 5), "1_8": (1, 8), "6_11": (6, 11), "12_35": (12, 35), "12_59": (12, 59)}


def _month_sql() -> str:
    """Per (eom, line) month-offset pivots of the line months (``k`` = months before the formation month)."""
    parts = [f"max(adj_last) FILTER (WHERE k = {o}) AS a{o}" for o in _MONTH_OFFSETS]
    parts += [f"max(n_obs) FILTER (WHERE k = {o}) AS o{o}" for o in (0, 11, 23, 35, 47, 59)]
    parts += [f"coalesce(sum(n_obs) FILTER (WHERE k BETWEEN {a} AND {b}), 0) AS obs_{n}"
              for n, (a, b) in _OBS_RANGES.items()]
    parts += ["coalesce(sum(n_ret) FILTER (WHERE k BETWEEN 1 AND 11), 0) AS ret_1_11",
              "coalesce(sum(n_pos) FILTER (WHERE k BETWEEN 1 AND 11), 0) AS pos_1_11",
              "coalesce(sum(n_neg) FILTER (WHERE k BETWEEN 1 AND 11), 0) AS neg_1_11"]
    parts += [f"sum({c}) FILTER (WHERE k BETWEEN 0 AND 59) AS b_{c}" for c in ("n3", "s3_x", "s3_m", "s3_xm",
                                                                                "s3_xx", "s3_mm")]
    return ",\n               ".join(parts)


def _week_sql() -> str:
    parts = ["count(*) AS w_n", "sum(y) AS w_y", "sum(y * y) AS w_yy"]
    for i in range(5):
        parts.append(f"sum(x{i}) AS w_x{i}")
        parts.append(f"sum(y * x{i}) AS w_xy{i}")
        for j in range(i, 5):
            parts.append(f"sum(x{i} * x{j}) AS w_xx{i}{j}")
    return ",\n               ".join(parts)


def stage_natives(work: Path, year: int, buckets: int, groups: int = 4) -> dict[str, Any]:
    """``natives/year=Y.parquet``: every native's raw value and status at the year's month ends."""
    import pyarrow.parquet as pq

    started = time.perf_counter()
    out = work / "natives" / f"year={year}.parquet"
    receipt = out.with_suffix(".json")
    code = natives_code_digest()
    inputs = {"bars": _spine.read_json(work / "lines.json")["inputs"]["bars"],
              "base": sha256_file(work / "base" / f"year={year}.parquet"),
              "market": sha256_file(work / "market.parquet"),
              "line_month": _spine.files_digest(work, "line_month/year=*.parquet"), "year": year,
              "buckets": buckets, "groups": groups}
    if _spine.receipt_ok(receipt, code, inputs):
        return _spine.read_json(receipt)
    forms = [f for f in _spine.year_formations(year) if f[0] >= _spine.FIRST_EOM]
    if not forms:
        raise ValueError(f"no formations in {year}")
    name = f"natives-{year}"
    con = _spine.connect(work, name)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(f".{out.name}.{os.getpid()}.tmp")
    writer = None
    counts: dict[str, dict[str, int]] = {}
    peak_rows = 0
    try:
        _spine.stage_sessions_table(con)
        fsno_lo, fsno_hi = min(f[2] for f in forms), max(f[2] for f in forms)
        lo, hi = fsno_lo - 252, fsno_hi
        con.execute(f"""
            CREATE TABLE mk AS
            SELECT x.sno, k.m, k.lm, lag(k.m) OVER w AS m_prev, lead(k.m) OVER w AS m_next
            FROM _px_sessions x LEFT JOIN read_parquet({sql_text((work / 'market.parquet').as_posix())}) k
              ON k.sno = x.sno
            WHERE x.sno BETWEEN {lo - 400} AND {hi + 5}
            WINDOW w AS (ORDER BY x.sno)
        """)
        sigma_m = {int(fsno): value for fsno, value in con.execute("""
            SELECT f.fsno, stddev_samp(k.lm) FROM (SELECT unnest(?::INTEGER[]) AS fsno) f
            JOIN mk k ON k.sno BETWEEN f.fsno - 251 AND f.fsno GROUP BY 1
        """, [[f[2] for f in forms]]).fetchall()}
        # Weekly EW market (compounded daily returns of complete weeks) and its four lags.
        con.execute(f"""
            CREATE TABLE mw AS
            SELECT week, wk_last_sno,
                   m0, CASE WHEN lag(week, 1) OVER w = week - 7 THEN lag(m0, 1) OVER w END AS x1,
                   CASE WHEN lag(week, 2) OVER w = week - 14 THEN lag(m0, 2) OVER w END AS x2,
                   CASE WHEN lag(week, 3) OVER w = week - 21 THEN lag(m0, 3) OVER w END AS x3,
                   CASE WHEN lag(week, 4) OVER w = week - 28 THEN lag(m0, 4) OVER w END AS x4
            FROM (SELECT x.week, max(x.sno) AS wk_last_sno,
                         CASE WHEN count(k.lm) = count(*) THEN exp(sum(k.lm)) - 1.0 END AS m0
                  FROM _px_sessions x LEFT JOIN mk k ON k.sno = x.sno
                  WHERE x.sno BETWEEN {lo - 400} AND {hi + 5}
                  GROUP BY x.week)
            WINDOW w AS (ORDER BY week)
        """)
        base = sql_text((work / "base" / f"year={year}.parquet").as_posix())
        line_months = _spine.parquet_list(sorted(p.as_posix() for p in (work / "line_month").glob("year=*.parquet")
                                                 if year - 6 <= int(p.stem.split("=", 1)[1]) <= year))
        for group in range(groups):
            members = [b for b in range(buckets) if b % groups == group]
            files = _spine.bars_files(work, members)
            bucket_sql = (f"(CAST(substr(line_id, {len(_spine.LINE_PREFIX) + 1}) AS BIGINT) % {buckets}) "
                          f"% {groups} = {group}")
            con.execute(f"""
                CREATE OR REPLACE TABLE u AS
                SELECT eom, line_id, formation_date, fsno, has_session_bar, price, adj AS adj_f, fac_f, fac_lag,
                       shares_lag_raw, shares_lagged, me_line, year(eom) * 12 + month(eom) AS mi
                FROM read_parquet({base}) WHERE eom >= DATE '{_spine.FIRST_EOM}' AND {bucket_sql}
            """)
            con.execute(f"""
                CREATE OR REPLACE TABLE d AS
                SELECT line_id, sno, close, adj, high, low, volume, r, lr, fac, m, m_prev, m_next,
                       {_pairs_sql()}
                FROM (
                  SELECT b.line_id, b.sno, b.close, b.adj, b.high, b.low, b.volume, b.r, b.lr, b.adj / b.close AS fac,
                         k.m, k.m_prev, k.m_next,
                         lag(b.sno) OVER w AS p_sno, lag(b.high) OVER w AS p_high, lag(b.low) OVER w AS p_low,
                         lag(b.close) OVER w AS p_close, lag(b.adj / b.close) OVER w AS p_fac
                  FROM read_parquet({_spine.parquet_list(files)}) b LEFT JOIN mk k ON k.sno = b.sno
                  WHERE b.sno BETWEEN {lo - 1} AND {hi}
                  WINDOW w AS (PARTITION BY b.line_id ORDER BY b.sno))
                WHERE sno >= {lo}
            """)
            daily = con.execute(f"""
                SELECT eom, line_id,
               {_daily_sql(fsno_lo, fsno_hi)}
                FROM (SELECT u.eom, u.line_id, u.fsno - d.sno AS g, u.fac_lag, d.* EXCLUDE (line_id)
                      FROM u JOIN d ON d.line_id = u.line_id AND d.sno BETWEEN u.fsno - 251 AND u.fsno
                      WHERE u.has_session_bar)
                GROUP BY eom, line_id
            """).fetchnumpy()
            month = con.execute(f"""
                SELECT eom, line_id,
               {_month_sql()}
                FROM (SELECT u.eom, u.line_id, u.mi - l.lmi AS k, l.adj_last, l.n_obs, l.n_ret, l.n_pos, l.n_neg,
                             l.n3, l.s3_x, l.s3_m, l.s3_xm, l.s3_xx, l.s3_mm
                      FROM u JOIN (SELECT *, year(eom) * 12 + month(eom) AS lmi
                                   FROM read_parquet({line_months})) l
                        ON l.line_id = u.line_id AND l.lmi BETWEEN u.mi - 60 AND u.mi
                      WHERE u.has_session_bar)
                GROUP BY eom, line_id
            """).fetchnumpy()
            week = con.execute(f"""
                WITH lw AS (
                  SELECT line_id, week, adj_w,
                         CASE WHEN lag(week) OVER w = week - 7 THEN adj_w / lag(adj_w) OVER w - 1.0 END AS y
                  FROM (SELECT b.line_id, x.week, arg_max(b.adj, b.sno) AS adj_w
                        FROM read_parquet({_spine.parquet_list(files)}) b JOIN _px_sessions x ON x.sno = b.sno
                        WHERE b.sno BETWEEN {lo - 400} AND {hi}
                        GROUP BY b.line_id, x.week)
                  WINDOW w AS (PARTITION BY line_id ORDER BY week))
                SELECT u.eom, u.line_id,
               {_week_sql()}
                FROM u JOIN lw ON lw.line_id = u.line_id
                JOIN (SELECT week, wk_last_sno, m0 AS x0, x1, x2, x3, x4 FROM mw) m ON m.week = lw.week
                WHERE u.has_session_bar AND m.wk_last_sno <= u.fsno AND lw.week > u.formation_date - 364
                  AND lw.y IS NOT NULL AND m.x0 IS NOT NULL AND m.x1 IS NOT NULL AND m.x2 IS NOT NULL
                  AND m.x3 IS NOT NULL AND m.x4 IS NOT NULL
                GROUP BY u.eom, u.line_id
            """).fetchnumpy()
            universe = con.execute("SELECT * FROM u ORDER BY eom, line_id").fetchnumpy()
            table = _compute(universe, daily, month, week, sigma_m)
            peak_rows = max(peak_rows, table.num_rows)
            for native in NATIVES:
                bucket = counts.setdefault(native.feature_id, {})
                for status, n in zip(*np.unique(np.asarray(table.column(native.feature_id + "__s").to_pylist(),
                                                           dtype=object), return_counts=True), strict=True):
                    bucket[str(status)] = bucket.get(str(status), 0) + int(n)
            if writer is None:
                writer = pq.ParquetWriter(tmp, table.schema, compression="zstd")
            writer.write_table(table)
            con.execute("DROP TABLE d")
        if writer is not None:
            writer.close()
            writer = None
    except BaseException:
        if writer is not None:
            writer.close()
        if tmp.exists():
            tmp.unlink()
        raise
    finally:
        con.close()
        _spine.drop_scratch(work, name)
    os.replace(tmp, out)
    rows = pq.read_metadata(out).num_rows
    return _spine.finish_receipt(receipt, code, inputs, [out], {"rows": rows, "status_counts": counts,
                                                                "max_group_rows": peak_rows}, started)


def _col(frame: Mapping[str, Any], name: str) -> np.ndarray:
    data = frame[name]
    if np.ma.isMaskedArray(data):
        return np.ma.filled(data.astype(float), np.nan)
    return np.asarray(data, dtype=float)


def _align(universe: Mapping[str, Any], part: Mapping[str, Any]) -> dict[str, np.ndarray]:
    """``part``'s columns reindexed to the universe's (eom, line_id) rows (NaN where absent)."""
    keys = {key: i for i, key in enumerate(zip(universe["eom"].tolist(), universe["line_id"].tolist(), strict=True))}
    n = len(universe["line_id"])
    rows = np.fromiter((keys[key] for key in zip(part["eom"].tolist(), part["line_id"].tolist(), strict=True)),
                       dtype=np.int64, count=len(part["line_id"]))
    out: dict[str, np.ndarray] = {}
    for name in part:
        if name in ("eom", "line_id"):
            continue
        column = np.full(n, np.nan)
        column[rows] = _col(part, name)
        out[name] = column
    return out


def _ratio(num: np.ndarray, den: np.ndarray) -> np.ndarray:
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.where(np.isfinite(num) & np.isfinite(den) & (den != 0), num / den, np.nan)


def _capm(n: np.ndarray, sx: np.ndarray, sy: np.ndarray, sxx: np.ndarray, syy: np.ndarray, sxy: np.ndarray
          ) -> tuple[np.ndarray, np.ndarray]:
    """(beta, residual sd) of y on x from the sums (n - 2 degrees of freedom)."""
    with np.errstate(divide="ignore", invalid="ignore"):
        cxx = sxx - sx * sx / n
        cxy = sxy - sx * sy / n
        cyy = syy - sy * sy / n
        beta = _ratio(cxy, cxx)
        resid = (cyy - beta * cxy) / (n - 2.0)
        sd = np.sqrt(np.where(resid > 0, resid, 0.0))
    return beta, np.where(np.isfinite(beta) & (n > 2), sd, np.nan)


def _solve(xtx: np.ndarray, xty: np.ndarray) -> np.ndarray:
    """Batched normal equations (NaN where a system is singular or not finite)."""
    out = np.full(xty.shape, np.nan)
    finite = np.isfinite(xtx).all(axis=(1, 2)) & np.isfinite(xty).all(axis=1)
    if not finite.any():
        return out
    index = np.flatnonzero(finite)
    cond = np.linalg.cond(xtx[index])
    good = index[np.isfinite(cond) & (cond < 1e12)]
    if len(good):
        out[good] = np.linalg.solve(xtx[good], xty[good][..., None])[..., 0]
    return out


def _dimson(a: Mapping[str, np.ndarray]) -> np.ndarray:
    n = a["k_n"]
    size = len(n)
    xtx = np.zeros((size, 4, 4))
    xty = np.zeros((size, 4))
    xtx[:, 0, 0] = n
    xty[:, 0] = a["k_y"]
    for i in range(3):
        xtx[:, 0, i + 1] = xtx[:, i + 1, 0] = a[f"k_x{i}"]
        xty[:, i + 1] = a[f"k_xy{i}"]
        for j in range(i, 3):
            xtx[:, i + 1, j + 1] = xtx[:, j + 1, i + 1] = a[f"k_xx{i}{j}"]
    coef = _solve(xtx, xty)
    return coef[:, 1] + coef[:, 2] + coef[:, 3]


def _price_delay(a: Mapping[str, np.ndarray]) -> np.ndarray:
    n = a["w_n"]
    size = len(n)
    xtx = np.zeros((size, 6, 6))
    xty = np.zeros((size, 6))
    xtx[:, 0, 0] = n
    xty[:, 0] = a["w_y"]
    for i in range(5):
        xtx[:, 0, i + 1] = xtx[:, i + 1, 0] = a[f"w_x{i}"]
        xty[:, i + 1] = a[f"w_xy{i}"]
        for j in range(i, 5):
            xtx[:, i + 1, j + 1] = xtx[:, j + 1, i + 1] = a[f"w_xx{i}{j}"]
    coef = _solve(xtx, xty)
    with np.errstate(divide="ignore", invalid="ignore"):
        sst = a["w_yy"] - a["w_y"] ** 2 / n
        explained = np.einsum("ij,ij->i", coef, xty) - a["w_y"] ** 2 / n
        r2_u = explained / sst
        cxx = a["w_xx00"] - a["w_x0"] ** 2 / n
        cxy = a["w_xy0"] - a["w_x0"] * a["w_y"] / n
        r2_r = cxy * cxy / (cxx * sst)
        return np.where((r2_u > 0) & np.isfinite(r2_r), 1.0 - r2_r / r2_u, np.nan)


def _coskew(a: Mapping[str, np.ndarray]) -> np.ndarray:
    n, sx, sy, sxx, syy, sxy, sxxx, sxxy = (a[k] for k in ("c_n", "c_sx", "c_sy", "c_sxx", "c_syy", "c_sxy",
                                                            "c_sxxx", "c_sxxy"))
    with np.errstate(divide="ignore", invalid="ignore"):
        mx, my = sx / n, sy / n
        cmm = sxx - n * mx * mx
        crm = sxy - n * my * mx
        crr = syy - n * my * my
        m3 = sxxx - 3.0 * mx * sxx + 3.0 * mx * mx * sx - n * mx ** 3
        srmm = (sxxy - 2.0 * mx * sxy + mx * mx * sy) - my * cmm
        beta = crm / cmm
        num = (srmm - beta * m3) / n
        e2 = (crr - beta * crm) / n
        den = np.sqrt(np.where(e2 > 0, e2, np.nan)) * (cmm / n)
        return np.where(np.isfinite(num / den), num / den, np.nan)


def _compute(universe: Mapping[str, Any], daily: Mapping[str, Any], month: Mapping[str, Any],
             week: Mapping[str, Any], sigma_m: Mapping[int, float]) -> Any:
    """Every native's value and status for the universe rows of one line group."""
    import pyarrow as pa

    n = len(universe["line_id"])
    a = {**_align(universe, daily), **_align(universe, month), **_align(universe, week)}
    for key in ("n_obs_21", "n_obs_252", "k_n", "w_n", "c_n", "c21_n", "d_n", "obs_1_11", "b_n3"):
        a.setdefault(key, np.full(n, np.nan))
    has_bar = np.asarray(universe["has_session_bar"], dtype=bool)
    price, adj_f = _col(universe, "price"), _col(universe, "adj_f")
    # Turnover: day t's volume restated to the lag date's share basis (fac_lag / fac_t in SQL) over the raw
    # lagged count, i.e. volume_t / (the lagged count in day t's basis).
    shares, me = _col(universe, "shares_lag_raw"), _col(universe, "me_line")
    shares = np.where(np.isfinite(_col(universe, "shares_lagged")), shares, np.nan)
    fsno = np.asarray(universe["fsno"], dtype=np.int64)
    values: dict[str, np.ndarray] = {}
    obs: dict[str, np.ndarray] = {}
    missing: dict[str, np.ndarray] = {}
    zeros = np.zeros(n, dtype=bool)

    def put(fid: str, value: np.ndarray, count: np.ndarray, miss: np.ndarray = zeros) -> None:
        values[fid], obs[fid], missing[fid] = value, np.nan_to_num(count, nan=0.0), miss

    r = _ratio
    ret_12_1 = r(a["a1"], a["a12"]) - 1.0
    put("ret_12_1", ret_12_1, a["obs_1_11"])
    put("ret_6_1", r(a["a1"], a["a6"]) - 1.0, a["obs_1_5"])
    put("ret_9_1", r(a["a1"], a["a9"]) - 1.0, a["obs_1_8"])
    put("ret_12_7", r(a["a6"], a["a12"]) - 1.0, a["obs_6_11"])
    put("ret_36_13", r(a["a12"], a["a36"]) - 1.0, a["obs_12_35"])
    put("ret_60_13", r(a["a12"], a["a60"]) - 1.0, a["obs_12_59"])
    put("chmom", (r(a["a1"], a["a6"]) - 1.0) - (r(a["a7"], a["a12"]) - 1.0), a["obs_1_11"])
    frog = np.sign(ret_12_1) * r(a["neg_1_11"] - a["pos_1_11"], a["ret_1_11"])
    put("frog_in_pan", np.where(np.isfinite(ret_12_1), frog, np.nan), a["ret_1_11"])
    seas1 = np.where(np.nan_to_num(a["o11"]) >= 15, r(a["a11"], a["a12"]) - 1.0, np.nan)
    put("seas_1_1an", seas1, a["o11"])
    legs = np.vstack([np.where(np.nan_to_num(a[f"o{j}"]) >= 15, r(a[f"a{j}"], a[f"a{j + 1}"]) - 1.0, np.nan)
                      for j in (23, 35, 47, 59)])
    seas_n = np.isfinite(legs).sum(axis=0).astype(float)
    with np.errstate(divide="ignore", invalid="ignore"):
        put("seas_2_5an", np.where(seas_n > 0, np.where(np.isfinite(legs), legs, 0.0).sum(axis=0) / seas_n, np.nan),
            seas_n)
    put("ret_1_0", r(adj_f, a["a1"]) - 1.0, a["o0"])
    put("rvol_21d", a["rvol_21"], a["n_lr_21"])
    put("rvol_252d", a["rvol_252"], a["n_lr_252"])
    put("rmax5_21d", a["rmax5_21"], a["n_r_21"])
    put("rmax1_21d", a["rmax1_21"], a["n_r_21"])
    put("rskew_252d", a["rskew_252"], a["n_r_252"])
    beta, ivol = _capm(a["c_n"], a["c_sx"], a["c_sy"], a["c_sxx"], a["c_syy"], a["c_sxy"])
    put("beta_ew_252d", beta, a["c_n"])
    put("ivol_ew_252d", ivol, a["c_n"])
    _, ivol21 = _capm(a["c21_n"], a["c21_sx"], a["c21_sy"], a["c21_sxx"], a["c21_syy"], a["c21_sxy"])
    put("ivol_ew_21d", ivol21, a["c21_n"])
    put("beta_dimson_252d", _dimson(a), a["k_n"])
    down, _ = _capm(a["d_n"], a["d_sx"], a["d_sy"], a["d_sxx"], a["d_sxx"], a["d_sxy"])
    put("beta_down_252d", down, a["d_n"])
    put("coskew_252d", _coskew(a), a["c_n"])
    with np.errstate(divide="ignore", invalid="ignore"):
        bn = a["b_n3"]
        cxm = a["b_s3_xm"] - a["b_s3_x"] * a["b_s3_m"] / bn
        cxx = a["b_s3_xx"] - a["b_s3_x"] ** 2 / bn
        cmm = a["b_s3_mm"] - a["b_s3_m"] ** 2 / bn
        rho = cxm / np.sqrt(cxx * cmm)
    sd_m = np.array([sigma_m.get(int(s), np.nan) for s in fsno], dtype=float)
    sd_i = np.where(np.nan_to_num(a["n_lr_252"]) >= 120, a["rvol_252"], np.nan)
    put("beta_bab_1260d", np.where(np.isfinite(rho), rho * r(sd_i, sd_m), np.nan), bn)
    put("zero_trade_21d", r(a["n_zero_21"], a["n_vol_21"]), a["n_vol_21"])
    put("zero_trade_252d", r(a["n_zero_252"], a["n_vol_252"]), a["n_vol_252"])
    no_shares = ~(np.isfinite(shares) & (shares > 0))
    put("turnover_126d", r(a["turn_126"], shares), a["n_vol_126"], no_shares)
    put("turnover_252d", r(a["turn_252"], shares), a["n_vol_252"], no_shares)
    put("std_turn_126d", r(a["sdturn_126"], shares), a["n_vol_126"], no_shares)
    put("std_dvol_126d", a["sddvol_126"], a["n_vol_126"])
    put("ami_126d", a["ami_126"] * AMIHUD_SCALE, a["n_ami_126"])
    put("ami_252d", a["ami_252"] * AMIHUD_SCALE, a["n_ami_252"])
    put("bidask_cs_21d", a["cs_21"], a["n_cs_21"])
    ar = a["ar_21"]
    with np.errstate(invalid="ignore"):
        put("bidask_ar_21d", np.where(np.isfinite(ar), np.sqrt(np.where(ar > 0, ar, 0.0)), np.nan), a["n_ar_21"])
    put("prc_log", price, has_bar.astype(float))
    put("prc_highprc_252d", r(adj_f, a["high_252"]), a["n_obs_252"])
    put("me_line_log", me, has_bar.astype(float), ~(np.isfinite(me)))
    put("dolvol_126d", a["dvol_126"], a["n_vol_126"])
    put("price_delay_52w", _price_delay(a), a["w_n"])

    def days(name: str) -> Any:
        return pa.array(np.asarray(universe[name]).astype("datetime64[D]"), type=pa.date32())

    columns: dict[str, Any] = {
        "eom": days("eom"), "line_id": pa.array(np.asarray(universe["line_id"], dtype=object), type=pa.string()),
        "formation_date": days("formation_date"), "has_session_bar": pa.array(has_bar, type=pa.bool_())}
    for native in NATIVES:
        fid = native.feature_id
        value, count, miss = values[fid], obs[fid], missing[fid]
        status = np.full(n, "in_domain", dtype=object)
        # NaN: the window statistic could not be formed (an endpoint or pair missing, a degenerate
        # window); +-inf: formed but not finite.
        status[np.isnan(value)] = "insufficient_obs"
        status[np.isinf(value)] = "nonfinite_value"
        if native.positive:
            status[np.isfinite(value) & (value <= 0)] = "nonpositive_value"
        status[count < native.min_obs] = "insufficient_obs"
        status[miss] = "missing_input"
        status[~has_bar] = "no_session_bar"
        value = np.where(np.isin(status, ("insufficient_obs", "missing_input", "no_session_bar", "nonfinite_value")),
                         np.nan, value)
        columns[fid] = pa.array(value, type=pa.float64(), from_pandas=True)
        columns[fid + "__s"] = pa.array(status.astype(str), type=pa.string())
        columns[fid + "__n"] = pa.array(count.astype(np.int32), type=pa.int32())
    return pa.table(columns)


# ---------------------------------------------------------------------------
# Feature store build
# ---------------------------------------------------------------------------

def native_input_digests(work: Path, th3: Path) -> dict[str, str]:
    return {"tickerhistory3": _spine.th3_sha256(th3), "natives": _spine.files_digest(work, "natives/year=*.parquet"),
            "spine": _spine.files_digest(work, "base/year=*.parquet")}


def build_store(work: Path, feature_ids: Sequence[str], root: Path, th3: Path) -> dict[str, Any]:
    """Write the natives into the Parquet feature store (basis ``reconstructed``); returns their entries."""
    import pandas as pd
    import pyarrow.parquet as pq

    from . import feature_store as fs
    from . import features as rf
    from .catalog import load_anomaly_catalog

    catalog = {entry.feature_id: entry for entry in load_anomaly_catalog() if entry.wave == WAVE}
    policy = rf.StandardizationPolicy()
    digests = native_input_digests(work, th3)
    code = fs.store_code_digest(Path(__file__), Path(_spine.__file__), Path(_spine._vendor_artifact.__file__),
                                Path(_spine._xnys.__file__))
    store = fs.FeatureStore(root)
    files = sorted((work / "natives").glob("year=*.parquet"))
    entries = []
    for fid in feature_ids:
        entry = catalog.get(fid)
        if entry is None:
            raise KeyError(f"{fid} is not a {WAVE} catalog row")
        started = time.perf_counter()
        sha = fs.compute_feature_sha(fid, fs.catalog_row_payload(entry, policy), code, digests)

        def chunks(fid: str = fid) -> Iterable[pd.DataFrame]:
            for path in files:
                table = pq.read_table(path, columns=["eom", "line_id", "formation_date", fid, fid + "__s"])
                frame = pd.DataFrame({
                    "eom": pd.to_datetime(table.column("eom").to_pandas()),
                    "line_id": table.column("line_id").to_pandas(), "owner_id": None,
                    "raw": table.column(fid).to_pandas(), "status": table.column(fid + "__s").to_pandas(),
                    "available_at": pd.to_datetime(table.column("formation_date").to_pandas())
                    + pd.Timedelta(hours=fs.DECISION_HOUR)})
                yield frame

        path = fs.build_feature(store, STORE_BASIS, fid, sha, chunks(), expected_sign=int(entry.expected_sign),
                                log_base=entry.preferred_transform == "log_winsor_z", policy=policy,
                                meta={"wave": WAVE, "native": NATIVES_VERSION, "definition": NATIVE_BY_ID[fid].expression,
                                      "min_obs": NATIVE_BY_ID[fid].min_obs, "inputs": digests,
                                      "universe_basis": _spine.UNIVERSE_BASIS})
        meta = store.read_meta(STORE_BASIS, fid, sha)
        entries.append({"basis": STORE_BASIS, "feature_id": fid, "feature_sha": sha, "path": path.as_posix(),
                        "rows": meta["rows"], "value_rows": meta["value_rows"], "reasons": meta["reasons"],
                        "bytes": meta["bytes"], "seconds": round(time.perf_counter() - started, 2)})
    store.close()
    return {"entries": entries}


__all__ = [
    "NATIVES",
    "NATIVES_VERSION",
    "NATIVE_BY_ID",
    "PRODUCER",
    "REQUIRES_STORE",
    "STORE_BASIS",
    "WAVE",
    "Native",
    "build_store",
    "natives_code_digest",
    "panel_native_specs",
    "stage_natives",
]
