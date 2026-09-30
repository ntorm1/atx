"""Stage C v2: raw daily characteristics from the panel, registry-driven and point in time by construction and by test.

Every feature is an entry of ``char_registry.REGISTRY`` (name, family, SQL, prior sign, citation, inputs, lookback,
kind, fill_zero). Values are raw (unranked), prior-signed (higher = predicted higher return) except controls (natural
sign). ``r`` is the panel ``ret`` with guarded returns treated as missing; ``m`` is ``mkt_ret``.

Point in time (Global Constraint 4): a value at session d uses only rows of the same line with ``session_date <= d``
(window frames end at the current row; no ``lead``, no ``FOLLOWING``, no whole-history statistic) and same-session
cross sections; every ``iv_atm_*`` input is the line's previous-session value (ruling R3). The vendor ``earnFlag``
is not used (its ``-1`` state marks the session BEFORE an announcement: look-ahead); event time comes from the SEC
8-K 2.02 columns ``earn_recent`` / ``earn_day_offset`` / ``earn_next_expected_date``. ``pit_check`` recomputes a
bucket at a decision session from rows ``<= d`` only and compares with the build.

Windows are over the line's own sessions and are NULL unless they span exactly the required number of calendar
sessions (the line traded on every session of the window), except where a registry note says otherwise
(``seas_annual_avg_2_5`` uses calendar-session RANGE frames). Lookbacks beyond the panel start (2018-01-02) read the
``prices_history`` stage (2012-03-26..2017-12-29, same ``security_id`` and ``ret`` / guard rules). Cross-sectional
group statistics use the same session's ``member`` lines of the FF49 group / FF12 sector with at least 3 values.

Sub-stages (resumable; a signature of the code and input manifests invalidates progress):

``extract``  per panel year: the narrow input columns joined with ``borrow_proxy`` on (session_date, security_id),
             written by line bucket (``security_id % NB``) -> ``_tmp/char_in/panel/year=YYYY/b=N/``; the
             ``prices_history`` return columns -> ``_tmp/char_in/hist/b=N/``; SPY monthly returns -> ``mkt_month``.
``line``     per bucket: every time-series term -> ``_tmp/char_line/b=N/yr=YYYY/`` (kept for ``pit_check``).
``year``     per year: same-session group statistics and the registry expressions ->
             ``characteristics/year=YYYY/characteristics.parquet`` keyed (session_date, security_id) with
             ``member, member_equity, grp_ff49, grp_ff12`` and every registry feature (DOUBLE; non-finite -> NULL;
             ``fill_zero`` features 0 when no event).
``manifest`` coverage (2020-2026 mean daily share of ``member_equity`` lines with a value) and the stage manifest
             with the registry embedded.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import sys
import time
from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import Any

from . import char_registry as R
from . import common as C

STAGE = "characteristics"
SCHEMA = "atx.alpha-panel.characteristics/v2"
MODULES = ("characteristics", "char_registry", "common")
NB = 64                       # line buckets: security_id % NB (16 ran out of a 600 MB DuckDB budget)
PART_FLUSH_ROWS = 65536       # partitioned COPY rows buffered before a flush (DuckDB default 524288)
PANEL_START = dt.date(2018, 1, 2)
MKT_ID = 549535               # SPY: market line for the monthly coskewness (2012+ in prices_history and panel)
PIT_REL_TOL = 1e-9
PIT_ABS_TOL = 1e-12
COVERAGE_YEARS = (2020, 2026)
IN_DIR = "char_in"
LINE_DIR = "char_line"
KEYS = ("session_date", "security_id")
FLAGS = ("member", "member_equity", "grp_ff49", "grp_ff12")

# Panel columns read by the line pass (Task 5 extends this with its fundamental inputs).
PANEL_COLUMNS = (
    "session_date", "security_id", "member", "member_equity", "grp_ff49", "grp_ff12",
    "open", "high", "low", "raw_close", "close", "ret", "ret_guarded", "volume", "dollar_volume", "adv63", "mkt_ret",
    "ret_intraday", "ret_overnight", "iv_atm_21d", "iv_atm_63d", "iv_atm_252d",
    "shares_out", "me_company", "me_line", "si_shares", "si_dtc", "sv_short_volume", "sv_total_volume",
    "ftd_quantity", "inst_shares", "inst_n_holders", "inst_d_holders", "inst_pct_change", "inst_top10_share",
    "earn_recent", "earn_day_offset", "earn_next_expected_date",
    "sue", "fscore", "txt_q", "txt_q_lag4", "at", "at_lag4", "ni_q", "be_lag1q", "ni_q_lag4", "be_lag1q_lag4", "noa",
    "shrs_q", "shrs_q_lag4", "ni_ttm", "cfo_ttm", "gp_ttm", "oi_ttm", "be", "sale_ttm", "debt", "che", "capx_ttm",
    "dvc_ttm", "prstkc_ttm", "sstk_ttm", "xrd_ttm", "sale_q", "sale_q_lag4",
)
# borrow_proxy column -> line column (clock < 22:00 UTC of d-1, borrow_proxy.py)
BP_COLUMNS = {"si_to_io": ("bp_si_to_io", "DOUBLE"), "ftd_quantity": ("bp_ftd_quantity", "DOUBLE"),
              "on_threshold_list": ("bp_on_threshold", "BOOLEAN"), "threshold_run_days": ("bp_run_days", "BIGINT")}
# same-session group means over member lines: (output, source term, group column); NULL below 3 values
GROUP_STATS = (
    ("g49_mom_12_1", "mom_12_1", "grp_ff49"), ("g49_mom_6_1", "mom_6_1", "grp_ff49"),
    ("g49_ret_5", "ret_5", "grp_ff49"), ("g49_ret_21", "ret_21", "grp_ff49"),
    ("g12_mom_12_1", "mom_12_1", "grp_ff12"),
)
GROUP_MIN = 3

_BY = R.by_name()
SCORECARD = {n: _BY[n].sql_or_callable for n in R.SCORECARD_NAMES}
EXTRA = {n: _BY[n].sql_or_callable for n in R.EXTRA_NAMES}

_LW = "(PARTITION BY security_id ORDER BY session_date)"


def _w(k: int) -> str:
    """The k rows ending at the current row."""
    return f"(PARTITION BY security_id ORDER BY session_date ROWS BETWEEN {k - 1} PRECEDING AND CURRENT ROW)"


def _wr(a: int, b: int) -> str:
    """Rows a..b before the current row (a >= b >= 1; the current row excluded)."""
    return f"(PARTITION BY security_id ORDER BY session_date ROWS BETWEEN {a} PRECEDING AND {b} PRECEDING)"


def _ws(a: int, b: int) -> str:
    """Calendar sessions sidx-a .. sidx-b of the line (RANGE over the session index)."""
    return f"(PARTITION BY security_id ORDER BY sidx RANGE BETWEEN {a} PRECEDING AND {b} PRECEDING)"


def _full(k: int) -> str:
    """The k-row window ending at the row spans exactly k consecutive sessions."""
    return f"(sidx - lag(sidx, {k - 1}) OVER {_LW} = {k - 1})"


def _gap(k: int) -> str:
    """The row k line rows back is exactly k sessions back."""
    return f"(sidx - lag(sidx, {k}) OVER {_LW} = {k})"


def _coskew(p: str) -> str:
    """Harvey-Siddique coskewness from the centred moment columns ``{p}cxx, cxy, cyy, cxxx, cxxy``:
    E[e x^2] / (sqrt(E[e^2]) E[x^2]) with e the market-model residual, x the demeaned market."""
    ee = f"({p}cyy - {p}cxy * {p}cxy / {p}cxx)"
    return (f"CASE WHEN {p}cxx > 0 AND {ee} > 0 "
            f"THEN ({p}cxxy - {p}cxy / {p}cxx * {p}cxxx) / (sqrt({ee}) * {p}cxx) END")


def _centred(p: str, cond: str) -> list[str]:
    """Centred second/third moments from raw window means ``{p}mx, my, mxx, mxy, myy, mxxx, mxxy``."""
    mx, my, mxx, mxy, myy, mxxx, mxxy = (f"{p}{s}" for s in ("mx", "my", "mxx", "mxy", "myy", "mxxx", "mxxy"))
    terms = {
        "cxx": f"{mxx} - {mx} * {mx}",
        "cxy": f"{mxy} - {mx} * {my}",
        "cyy": f"{myy} - {my} * {my}",
        "cxxx": f"{mxxx} - 3 * {mx} * {mxx} + 2 * {mx} * {mx} * {mx}",
        "cxxy": f"{mxxy} - {my} * {mxx} - 2 * {mx} * {mxy} + 2 * {mx} * {mx} * {my}",
    }
    return [f"CASE WHEN {cond} THEN {v} END AS {p}{k}" for k, v in terms.items()]


def _moments(p: str, x: str, y: str, win: str) -> list[str]:
    return [f"count({x}) OVER {win} AS {p}n", f"avg({x}) OVER {win} AS {p}mx", f"avg({y}) OVER {win} AS {p}my",
            f"avg({x} * {x}) OVER {win} AS {p}mxx", f"avg({x} * {y}) OVER {win} AS {p}mxy",
            f"avg({y} * {y}) OVER {win} AS {p}myy", f"avg({x} * {x} * {x}) OVER {win} AS {p}mxxx",
            f"avg({x} * {x} * {y}) OVER {win} AS {p}mxxy"]


# ------------------------------------------------------------------------------------------------ SQL builders

def cal_sql(cal_path: str) -> str:
    """Calendar CTEs: ``cal`` (session_date, sidx, month_idx, month_end) and ``cm`` (month_idx, n_month).
    ``month_end`` marks the last session of a calendar month (the schedule is public); the calendar's final month
    is never treated as complete."""
    return f"""
    cal0 AS (SELECT session_date, row_number() OVER (ORDER BY session_date) AS sidx,
                    year(session_date) * 12 + month(session_date) - 1 AS month_idx
             FROM read_parquet('{cal_path}')),
    cm AS (SELECT month_idx, count(*) AS n_month, max(session_date) AS last_session FROM cal0 GROUP BY 1),
    cal AS (SELECT c.session_date, c.sidx, c.month_idx,
                   (c.session_date = cm.last_session AND c.month_idx < (SELECT max(month_idx) FROM cal0)) AS month_end
            FROM cal0 c JOIN cm USING (month_idx))"""


def input_sql(panel_rel: str, bp_rel: str | None, columns: Sequence[str] = PANEL_COLUMNS) -> str:
    """The narrow line-pass input: panel columns plus the borrow_proxy columns (NULL when the stage is absent)."""
    cols = ", ".join(f'p."{c}"' for c in columns)
    if bp_rel is None:
        bp = ", ".join(f"CAST(NULL AS {t}) AS {out}" for out, t in BP_COLUMNS.values())
        return f"SELECT {cols}, {bp} FROM {panel_rel} p"
    bp = ", ".join(f"q.{src} AS {out}" for src, (out, _) in BP_COLUMNS.items())
    return f"SELECT {cols}, {bp} FROM {panel_rel} p LEFT JOIN {bp_rel} q USING (session_date, security_id)"


def mkt_month_sql(daily_rel: str, cal_path: str) -> str:
    """SPY monthly log return per calendar month (complete months only: an unguarded return on every session)."""
    return f"""
    WITH {cal_sql(cal_path)}
    SELECT c.month_idx, sum(ln(1 + x.r)) AS m_lr
    FROM (SELECT session_date, CASE WHEN ret_guarded OR ret <= -1 THEN NULL ELSE ret END AS r
          FROM {daily_rel} WHERE security_id = {MKT_ID}) x
    JOIN cal c USING (session_date) JOIN cm USING (month_idx)
    GROUP BY 1 HAVING count(x.r) = max(cm.n_month)"""


def next_sql(inp: str, cal_path: str) -> str:
    """Next expected announcement per row: the smallest ``earn_next_expected_date`` seen on the line's rows t-300..t
    that is after d and more than 30 days after the last reaction session (the current value projects the same
    quarter NEXT year). Columns: security_id, session_date, next_exp."""
    return f"""
    WITH {cal_sql(cal_path)},
    le AS (
        SELECT i.security_id, i.session_date, c.sidx,
               max(CASE WHEN i.earn_day_offset = 0 THEN i.session_date END) OVER (PARTITION BY i.security_id
                   ORDER BY i.session_date ROWS UNBOUNDED PRECEDING) AS last_earn_date
        FROM {inp} i JOIN cal c USING (session_date)
    ),
    ev AS (
        SELECT i.security_id, i.earn_next_expected_date AS ned, min(c.sidx) AS first_sidx
        FROM {inp} i JOIN cal c USING (session_date)
        WHERE i.earn_next_expected_date IS NOT NULL GROUP BY 1, 2
    )
    SELECT le.security_id, le.session_date, min(ev.ned) AS next_exp
    FROM le JOIN ev ON ev.security_id = le.security_id AND ev.first_sidx <= le.sidx AND ev.first_sidx >= le.sidx - 300
         AND ev.ned > le.session_date
         AND (le.last_earn_date IS NULL OR date_diff('day', le.last_earn_date, ev.ned) > 30)
    GROUP BY 1, 2"""


def long_sql(inp: str, hist: str | None, mkt_month: str | None, cal_path: str) -> str:
    """Long-history terms over prices_history (before the panel) + panel returns: ``seas_2_5`` (years 2-5 same-window
    returns, calendar-session RANGE frames) and ``coskew_60m`` (monthly, the 60 months ending with the last month
    completed at d). Columns: security_id, session_date, seas_2_5, coskew_60m (panel sessions only)."""
    hist_rows = (f"SELECT security_id, session_date, CASE WHEN ret_guarded THEN NULL ELSE ret END AS r FROM {hist} "
                 f"WHERE session_date < DATE '{PANEL_START}' UNION ALL " if hist else "")
    mk = mkt_month or "(SELECT CAST(NULL AS BIGINT) AS month_idx, CAST(NULL AS DOUBLE) AS m_lr WHERE false)"
    seas = []
    for k in (2, 3, 4, 5):
        a, b = 252 * k - 8, 252 * k - 28
        seas += [f"sum(lr) OVER {_ws(a, b)} AS s{k}", f"count(lr) OVER {_ws(a, b)} AS n{k}"]
    seas_avg = (" + ".join(f"coalesce(CASE WHEN n{k} = 21 THEN exp(s{k}) - 1 END, 0)" for k in (2, 3, 4, 5))
                + ") / nullif(" + " + ".join(f"CAST(n{k} = 21 AS INTEGER)" for k in (2, 3, 4, 5)) + ", 0)")
    wm = "(PARTITION BY security_id ORDER BY month_idx RANGE BETWEEN 59 PRECEDING AND CURRENT ROW)"
    return f"""
    WITH {cal_sql(cal_path)},
    lh AS ({hist_rows}SELECT security_id, session_date, CASE WHEN ret_guarded THEN NULL ELSE ret END AS r FROM {inp}),
    ld AS (SELECT lh.security_id, lh.session_date, c.sidx, c.month_idx, c.month_end,
                  CASE WHEN lh.r > -1 THEN ln(1 + lh.r) END AS lr
           FROM lh JOIN cal c USING (session_date)),
    ls AS (SELECT security_id, session_date, month_idx, month_end, {', '.join(seas)} FROM ld),
    lm AS (SELECT security_id, month_idx, sum(lr) AS mlr, count(lr) AS n FROM ld GROUP BY 1, 2),
    lm3 AS (
        SELECT l.security_id, l.month_idx,
               CASE WHEN l.n = cm.n_month THEN exp(k.m_lr) - 1 END AS x,
               CASE WHEN l.n = cm.n_month AND k.m_lr IS NOT NULL THEN exp(l.mlr) - 1 END AS y
        FROM lm l JOIN cm USING (month_idx) LEFT JOIN {mk} k USING (month_idx)
    ),
    lm4 AS (SELECT security_id, month_idx, {', '.join(_moments('m_', 'x', 'y', wm))} FROM lm3),
    lm5 AS (SELECT security_id, month_idx, {', '.join(_centred('m_', 'm_n >= 48'))} FROM lm4),
    lm6 AS (SELECT security_id, month_idx, {_coskew('m_')} AS coskew_60m FROM lm5)
    SELECT ls.security_id, ls.session_date, ({seas_avg} AS seas_2_5, lm6.coskew_60m
    FROM ls LEFT JOIN lm6 ON lm6.security_id = ls.security_id
         AND lm6.month_idx = CASE WHEN ls.month_end THEN ls.month_idx ELSE ls.month_idx - 1 END
    WHERE ls.session_date >= DATE '{PANEL_START}'"""


def line_sql(inp: str, hist: str | None, mkt_month: str | None, cal_path: str,
             extra_terms: dict[str, str] | None = None, long_rel: str | None = None,
             next_rel: str | None = None) -> str:
    """Every time-series term of one set of lines (a bucket). ``inp`` / ``hist`` / ``mkt_month`` are relations
    (``read_parquet(...)`` or views); ``long_rel`` / ``next_rel`` are the materialized results of ``long_sql`` /
    ``next_sql`` (inlined when None); ``extra_terms`` {name: window SQL over the final layer} is a test hook."""
    long_rel = long_rel or f"({long_sql(inp, hist, mkt_month, cal_path)})"
    next_rel = next_rel or f"({next_sql(inp, cal_path)})"
    a_terms = [
        f"{_full(3)} AS f3", f"{_full(5)} AS f5", f"{_full(21)} AS f21", f"{_full(55)} AS f55",
        f"{_full(63)} AS f63", f"{_full(126)} AS f126", f"{_full(250)} AS f250", f"{_full(252)} AS f252",
        *(f"{_gap(k)} AS g{k}" for k in (5, 6, 21, 22, 126, 147, 224, 245, 252)),
        *(f"lag(close, {k}) OVER {_LW} AS close_{k}" for k in (5, 21, 126, 147, 224, 245, 252)),
        f"lag(adj_shares, 252) OVER {_LW} AS adj_shares_252_raw",
        f"lag(si_ratio_raw, 21) OVER {_LW} AS si_ratio_21_raw",
        f"lag(gm_now, 252) OVER {_LW} AS gm_252",
        # IV: the line's previous session only (Global Constraint 4, ruling R3)
        f"lag(iv_atm_21d) OVER {_LW} AS iv21_l1", f"lag(iv_atm_21d, 6) OVER {_LW} AS iv21_l6",
        f"lag(iv_atm_63d) OVER {_LW} AS iv63_l1", f"lag(iv_atm_63d, 22) OVER {_LW} AS iv63_l22",
        f"lag(iv_atm_252d) OVER {_LW} AS iv252_l1",
        f"last_value(iv_atm_21d IGNORE NULLS) OVER {_wr(6, 1)} AS iv21_bf",
        f"sum(r) OVER {_w(3)} AS r3", f"sum(m) OVER {_w(3)} AS m3",
        f"stddev_samp(r) OVER {_w(21)} AS sd_r21", f"count(r) OVER {_w(21)} AS n_r21",
        f"corr(r, m) OVER {_w(21)} AS c_rm21", f"max(r) OVER {_w(21)} AS max_r21",
        f"stddev_samp(r) OVER {_w(63)} AS sd_r63", f"count(r) OVER {_w(63)} AS n_r63",
        f"corr(r, m) OVER {_w(63)} AS c_rm63", f"skewness(r) OVER {_w(63)} AS skew_r63",
        f"count(CASE WHEN r = 0 THEN 1 END) OVER {_w(63)} AS zero_n63",
        f"stddev_samp(r) OVER {_w(126)} AS sd_r126", f"count(r) OVER {_w(126)} AS n_r126",
        f"stddev_samp(r) OVER {_w(252)} AS sd_r252", f"count(r) OVER {_w(252)} AS n_r252",
        f"stddev_samp(m) OVER {_w(252)} AS sd_m252",
        *_moments("d_", "cx", "cy", _w(252)),
        f"regr_slope(dy, dx) OVER {_w(252)} AS dbeta_raw", f"regr_count(dy, dx) OVER {_w(252)} AS n_d252",
        f"max(close) OVER {_w(252)} AS max_close_252_raw",
        f"avg(volume) OVER {_w(5)} AS vol_avg_5", f"avg(volume) OVER {_wr(54, 5)} AS vol_avg_p50",
        f"avg(volume) OVER {_w(21)} AS vol_avg_21_raw", f"avg(volume) OVER {_w(252)} AS vol_avg_252",
        f"avg(CASE WHEN dv > 0 AND r IS NOT NULL THEN abs(r) / dv * 1e6 END) OVER {_w(21)} AS amihud_raw",
        f"stddev_samp(dv) OVER {_w(63)} AS sd_dv63", f"avg(dv) OVER {_w(63)} AS avg_dv63",
        f"count(dv) OVER {_w(63)} AS n_dv63",
        f"regr_slope(ldv, sidx) OVER {_w(252)} AS vtr_slope", f"regr_count(ldv, sidx) OVER {_w(252)} AS vtr_n",
        f"avg(svr_1) OVER {_w(5)} AS svr_5_raw", f"count(svr_1) OVER {_w(5)} AS n_svr5",
        f"avg(svr_1) OVER {_w(21)} AS svr_21_raw", f"count(svr_1) OVER {_w(21)} AS n_svr21",
        f"sum(ri) OVER {_w(5)} AS ri_5", f"count(ri) OVER {_w(5)} AS n_ri5",
        f"sum(ri) OVER {_w(21)} AS ri_21", f"count(ri) OVER {_w(21)} AS n_ri21",
        f"sum(ro) OVER {_w(21)} AS ro_21", f"count(ro) OVER {_w(21)} AS n_ro21",
        # formation window t-251..t-21 (the 12-1 momentum returns)
        f"count(r) OVER {_wr(251, 21)} AS fp_n", f"count(CASE WHEN r > 0 THEN 1 END) OVER {_wr(251, 21)} AS fp_pos",
        f"count(CASE WHEN r < 0 THEN 1 END) OVER {_wr(251, 21)} AS fp_neg",
        f"sum(ro) OVER {_wr(251, 21)} AS ovn_sum", f"count(ro) OVER {_wr(251, 21)} AS ovn_n",
        # SEC 8-K 2.02 reaction sessions (earn_day_offset = 0 is known by 22:00 UTC of that session)
        "max(CASE WHEN earn_day_offset = 0 THEN sidx END) OVER (PARTITION BY security_id ORDER BY session_date "
        "ROWS UNBOUNDED PRECEDING) AS last_earn_sidx",
        f"lag(earn_recent) OVER {_LW} AS earn_recent_prev",
        # Corwin-Schultz two-day inputs
        f"hl * hl + lag(hl) OVER {_LW} * lag(hl) OVER {_LW} AS cs_beta",
        f"CASE WHEN greatest(high, lag(high) OVER {_LW}) > 0 AND least(low, lag(low) OVER {_LW}) > 0 "
        f"THEN ln(greatest(high, lag(high) OVER {_LW}) / least(low, lag(low) OVER {_LW})) END AS cs_gamma_ln",
    ]
    extra = ""
    if extra_terms:
        extra = ", bx AS (SELECT *, " + ", ".join(f"{v} AS {k}" for k, v in extra_terms.items()) + " FROM b5)"
    last = "bx" if extra_terms else "b5"
    return f"""
    WITH {cal_sql(cal_path)},
    p AS (
        SELECT i.*, cal.sidx,
               CASE WHEN ret_guarded THEN NULL ELSE ret END AS r,
               mkt_ret AS m,
               -- (r, m) pairs: both present, else both NULL (the moment windows must see the same rows)
               CASE WHEN NOT ret_guarded AND ret IS NOT NULL AND mkt_ret IS NOT NULL THEN mkt_ret END AS cx,
               CASE WHEN NOT ret_guarded AND ret IS NOT NULL AND mkt_ret IS NOT NULL THEN ret END AS cy,
               CASE WHEN NOT ret_guarded AND ret IS NOT NULL AND mkt_ret < 0 THEN mkt_ret END AS dx,
               CASE WHEN NOT ret_guarded AND ret IS NOT NULL AND mkt_ret < 0 THEN ret END AS dy,
               CASE WHEN close > 0 AND raw_close > 0 AND shares_out > 0 THEN shares_out * raw_close / close END AS adj_shares,
               CASE WHEN shares_out > 0 THEN si_shares / shares_out END AS si_ratio_raw,
               CASE WHEN sv_total_volume > 0 THEN sv_short_volume / sv_total_volume END AS svr_1,
               CASE WHEN high > 0 AND low > 0 AND high >= low THEN ln(high / low) END AS hl,
               CASE WHEN NOT ret_guarded THEN ret_intraday END AS ri,
               ret_overnight AS ro,
               CASE WHEN dollar_volume > 0 THEN dollar_volume END AS dv,
               CASE WHEN dollar_volume > 0 THEN ln(dollar_volume) END AS ldv,
               CASE WHEN sale_ttm > 0 AND gp_ttm IS NOT NULL THEN gp_ttm / sale_ttm END AS gm_now
        FROM {inp} i JOIN cal USING (session_date)
    ),
    a AS (SELECT *, {', '.join(a_terms)} FROM p),
    b1 AS (
        SELECT *,
            CASE WHEN f250 THEN corr(r3, m3) OVER {_w(250)} END AS c_r3m3_250,
            CASE WHEN g21 AND g252 AND close_252 > 0 THEN close_21 / close_252 - 1 END AS mom_12_1,
            CASE WHEN g21 AND g126 AND close_126 > 0 THEN close_21 / close_126 - 1 END AS mom_6_1,
            CASE WHEN g147 AND g252 AND close_252 > 0 THEN close_147 / close_252 - 1 END AS mom_12_7,
            CASE WHEN g252 AND close_252 > 0 THEN close / close_252 - 1 END AS ret_252,
            CASE WHEN g5 AND close_5 > 0 THEN close / close_5 - 1 END AS ret_5,
            CASE WHEN g21 AND close_21 > 0 THEN close / close_21 - 1 END AS ret_21,
            CASE WHEN g224 AND g245 AND close_245 > 0 THEN close_224 / close_245 - 1 END AS seas_same_month,
            CASE WHEN g252 THEN adj_shares_252_raw END AS adj_shares_252,
            CASE WHEN g21 THEN si_ratio_21_raw END AS si_ratio_21,
            CASE WHEN g22 THEN iv63_l1 - iv63_l22 END AS iv63_chg21,
            CASE WHEN g6 THEN iv21_l1 - iv21_l6 END AS iv21_chg5,
            CASE WHEN f21 AND n_r21 >= 21 THEN sd_r21 END AS vol_21,
            CASE WHEN f21 AND n_r21 >= 21 THEN sd_r21 * sqrt(greatest(1 - c_rm21 * c_rm21, 0)) END AS ivol_21,
            CASE WHEN f21 AND n_r21 >= 21 THEN max_r21 END AS max_21,
            CASE WHEN f63 AND n_r63 >= 60 THEN sd_r63 END AS vol_63,
            CASE WHEN f63 AND n_r63 >= 60 THEN sd_r63 * sqrt(greatest(1 - c_rm63 * c_rm63, 0)) END AS ivol_63,
            CASE WHEN f63 AND n_r63 >= 60 THEN skew_r63 END AS skew_63,
            CASE WHEN f63 AND n_r63 >= 60 THEN CAST(zero_n63 AS DOUBLE) / n_r63 END AS zero_63,
            CASE WHEN f126 AND n_r126 >= 120 THEN sd_r126 END AS vol_126,
            CASE WHEN f252 AND n_r252 >= 240 THEN sd_r252 END AS vol_252,
            {', '.join(_centred('d_', 'f252 AND d_n >= 200'))},
            CASE WHEN f252 AND n_d252 >= 50 THEN dbeta_raw END AS dbeta_252,
            CASE WHEN f252 THEN max_close_252_raw END AS max_close_252,
            CASE WHEN f21 THEN vol_avg_21_raw END AS vol_avg_21,
            CASE WHEN f21 AND f252 AND vol_avg_252 > 0 THEN vol_avg_21_raw / vol_avg_252 - 1 END AS abn_turnover,
            CASE WHEN f55 AND vol_avg_p50 > 0 THEN vol_avg_5 / vol_avg_p50 - 1 END AS abn_vol_5,
            CASE WHEN f21 THEN amihud_raw END AS amihud_21,
            CASE WHEN f63 AND n_dv63 >= 60 AND avg_dv63 > 0 THEN sd_dv63 / avg_dv63 END AS cv_dv_63,
            CASE WHEN f252 AND vtr_n >= 200 THEN vtr_slope * 21 END AS vtrend_252,
            CASE WHEN n_svr5 >= 4 THEN svr_5_raw END AS svr_5,
            CASE WHEN n_svr21 >= 15 THEN svr_21_raw END AS svr_21,
            CASE WHEN f5 AND n_ri5 = 5 THEN ri_5 END AS ri_sum_5,
            CASE WHEN f21 AND n_ri21 = 21 THEN ri_21 END AS ri_sum_21,
            CASE WHEN f21 AND n_ro21 = 21 THEN ro_21 END AS ro_sum_21,
            CASE WHEN f252 AND g21 AND g252 AND close_252 > 0 AND fp_n >= 200
                 THEN sign(close_21 / close_252 - 1) * CAST(fp_neg - fp_pos AS DOUBLE) / fp_n END AS fip,
            CASE WHEN f252 AND ovn_n >= 200 THEN ovn_sum END AS ovn_12,
            CASE WHEN last_earn_sidx IS NOT NULL THEN CAST(sidx - last_earn_sidx AS DOUBLE) END AS earn_days_since,
            -- EAR: 3-session abnormal return ending on the session after the reaction day (earn_recent now and before)
            CASE WHEN earn_recent = 1 AND earn_recent_prev = 1 AND f3 THEN r3 - m3 END AS ear_point,
            CASE WHEN cs_beta IS NOT NULL AND cs_gamma_ln IS NOT NULL THEN
                 (sqrt(2 * cs_beta) - sqrt(cs_beta)) / (3 - 2 * sqrt(2))
                 - sqrt(cs_gamma_ln * cs_gamma_ln / (3 - 2 * sqrt(2))) END AS cs_alpha,
            gm_now - gm_252 AS gm_change
        FROM a
    ),
    b2 AS (
        SELECT *,
            CASE WHEN f252 AND c_r3m3_250 IS NOT NULL AND sd_m252 > 0 AND vol_252 IS NOT NULL
                 THEN c_r3m3_250 * vol_252 / sd_m252 END AS beta_252,
            {_coskew('d_')} AS coskew_252,
            last_value(ear_point IGNORE NULLS) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN 125 PRECEDING AND CURRENT ROW) AS ear_raw,
            avg(CASE WHEN cs_alpha IS NOT NULL THEN greatest(2 * (exp(cs_alpha) - 1) / (1 + exp(cs_alpha)), 0) END)
                OVER {_w(21)} AS hl_spread_raw
        FROM b1
    ),
    b3 AS (SELECT *, r - coalesce(beta_252, 1) * m AS resid_r, CASE WHEN f21 THEN hl_spread_raw END AS hl_spread_21
           FROM b2),
    b4 AS (
        SELECT *,
            -- residual momentum: sum of residual returns t-251..t-21 scaled by their stdev
            sum(resid_r) OVER {_wr(251, 21)} AS rs_sum, stddev_samp(resid_r) OVER {_wr(251, 21)} AS rs_sd,
            count(resid_r) OVER {_wr(251, 21)} AS rs_n
        FROM b3
    ),
    b5 AS (SELECT *, CASE WHEN f252 AND rs_n >= 200 AND rs_sd > 0 THEN rs_sum / rs_sd END AS resid_mom_12_1 FROM b4)
    {extra}
    SELECT {last}.*, nx.next_exp, CAST(cn.sidx - {last}.sidx AS INTEGER) AS ea_gap, lg.seas_2_5, lg.coskew_60m
    FROM {last} LEFT JOIN {next_rel} nx USING (security_id, session_date)
    LEFT JOIN cal cn ON cn.session_date = nx.next_exp
    LEFT JOIN {long_rel} lg USING (security_id, session_date)
    """


def line_query(con, inp: str, hist: str | None, mkt_month: str | None, cal_path: str,
               features: Sequence[R.Feature], extra_terms: dict[str, str] | None = None,
               tag: str = "lt") -> tuple[str, list[str]]:
    """Materialize the long-history and next-announcement terms as tables ``{tag}_long`` / ``{tag}_next`` (on disk in
    a file-backed connection: one window chain in memory at a time), then return the line query over them and the
    columns the year pass keeps."""
    con.execute(f"CREATE OR REPLACE TABLE {tag}_long AS {long_sql(inp, hist, mkt_month, cal_path)}")
    con.execute(f"CREATE OR REPLACE TABLE {tag}_next AS {next_sql(inp, cal_path)}")
    sql = line_sql(inp, hist, mkt_month, cal_path, extra_terms, long_rel=f"{tag}_long", next_rel=f"{tag}_next")
    keep = list(dict.fromkeys(keep_columns(_line_columns(con, sql), features) + list(extra_terms or {})))
    return sql, keep


_TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _feature_sql(f: R.Feature) -> str:
    if not isinstance(f.sql_or_callable, str):
        raise NotImplementedError(f"{f.name}: callable features are not supported by the v2 build")
    return f.sql_or_callable


def keep_columns(line_cols: Iterable[str], features: Sequence[R.Feature]) -> list[str]:
    """Line-pass columns the year pass needs: keys, flags, group-stat sources and every column a feature names."""
    have = list(line_cols)
    want = set(KEYS) | set(FLAGS) | {src for _, src, _ in GROUP_STATS}
    for f in features:
        want |= set(_TOKEN.findall(_feature_sql(f)))
    return [c for c in have if c in want]


def year_sql(line_rel: str, lo: dt.date, hi: dt.date, features: Sequence[R.Feature]) -> str:
    """Same-session group statistics and the registry expressions over line rows of sessions lo..hi."""
    y = f"(SELECT * FROM {line_rel} WHERE session_date BETWEEN DATE '{lo}' AND DATE '{hi}')"
    groups: dict[str, list[tuple[str, str]]] = {}
    for out, src, grp in GROUP_STATS:
        groups.setdefault(grp, []).append((out, src))
    ctes, joins, gcols = [], [], []
    for grp, stats in groups.items():
        aggs = ", ".join(f"avg({src}) AS a_{out}, count({src}) AS n_{out}" for out, src in stats)
        ctes.append(f"q_{grp} AS (SELECT session_date, {grp}, {aggs} FROM {y} WHERE member AND {grp} IS NOT NULL "
                    f"GROUP BY 1, 2)")
        joins.append(f"LEFT JOIN q_{grp} USING (session_date, {grp})")
        gcols += [f"CASE WHEN n_{out} >= {GROUP_MIN} THEN a_{out} END AS {out}" for out, _ in stats]
    raw = ", ".join(f"CAST(({_feature_sql(f)}) AS DOUBLE) AS \"{f.name}\"" for f in features)
    out_cols = []
    for f in features:
        v = f'CASE WHEN isfinite("{f.name}") THEN "{f.name}" END'
        out_cols.append(f'coalesce({v}, 0.0) AS "{f.name}"' if f.fill_zero else f'{v} AS "{f.name}"')
    flags = ", ".join(FLAGS)
    return f"""
    WITH {', '.join(ctes)},
    z AS (SELECT y.*, {', '.join(gcols)} FROM {y} y {' '.join(joins)}),
    z2 AS (SELECT session_date, security_id, {flags}, {raw} FROM z)
    SELECT session_date, security_id, {flags}, {', '.join(out_cols)} FROM z2
    """


def lint_build(features: Sequence[R.Feature]) -> list[str]:
    """Static look-ahead lint of the production line and year SQL (``char_registry.lint_sql``)."""
    problems = []
    sql = line_sql("inp", "hist", "mkt", "cal.parquet") + year_sql("line", dt.date(2020, 1, 1),
                                                                    dt.date(2020, 12, 31), features)
    for bad in R.lint_sql(sql):
        problems.append(f"line/year SQL: {bad}")
    for f in features:
        for bad in R.lint_sql(_feature_sql(f)):
            problems.append(f"{f.name}: {bad}")
    return problems


# ------------------------------------------------------------------------------------------------ build

def _rp(glob: str | Path) -> str:
    return f"read_parquet('{Path(glob).as_posix()}', hive_partitioning = false, union_by_name = true)"


def _tmp(*parts: str) -> Path:
    return C.build_root() / "_tmp" / Path(*parts)


def _input_manifests() -> dict[str, str | None]:
    root = C.build_root()
    out: dict[str, str | None] = {}
    for st in ("panel", "prices_history", "borrow_proxy"):
        m = root / st / "manifest.json"
        out[st] = C.sha256_file(m) if m.exists() else None
    out["calendar.parquet"] = C.sha256_file(C.calendar_path()) if C.calendar_path().exists() else None
    return out


def _signature(nb: int, features: Sequence[R.Feature], extra_terms: dict[str, str] | None) -> str:
    here = Path(__file__).resolve().parent
    h = hashlib.sha256()
    for m in ("characteristics.py", "char_registry.py"):
        h.update((here / m).read_bytes().replace(b"\r\n", b"\n"))
    h.update(json.dumps({"nb": nb, "inputs": _input_manifests(), "features": [f.as_dict() for f in features],
                         "extra": extra_terms or {}}, sort_keys=True, default=str).encode())
    return h.hexdigest()


def _replace_dir(partial: Path, final: Path) -> None:
    if final.exists():
        shutil.rmtree(final)
    os.replace(partial, final)


def _copy_partitioned(con, sql: str, dest: Path, part: str, row_group_size: int = 16384) -> None:
    partial = dest.with_name(dest.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    partial.parent.mkdir(parents=True, exist_ok=True)
    con.execute(f"SET partitioned_write_flush_threshold = {PART_FLUSH_ROWS}")
    con.execute(f"COPY ({sql}) TO '{partial.as_posix()}' (FORMAT PARQUET, COMPRESSION ZSTD, PARTITION_BY ({part}), "
                f"ROW_GROUP_SIZE {row_group_size})")
    _replace_dir(partial, dest)


def extract(con, years: Sequence[int], nb: int) -> dict[str, Any]:
    """Narrow per-bucket inputs from the panel (+ borrow_proxy) and prices_history; SPY monthly returns."""
    root = C.build_root()
    out: dict[str, Any] = {}
    panel_years = sorted(int(p.name.split("=")[1]) for p in (root / "panel").glob("year=*") if p.is_dir())
    # every panel year up to the last output year: the years before the first output year are window warm-up
    for d in _tmp(IN_DIR, "panel").glob("year=*"):
        if int(d.name.split("=")[1]) > max(years):
            shutil.rmtree(d)
    for y in [py for py in panel_years if py <= max(years)]:
        bp = root / "borrow_proxy" / f"year={y}"
        bp_rel = _rp(bp / "*.parquet") if list(bp.glob("*.parquet")) else None
        sql = input_sql(_rp(root / "panel" / f"year={y}" / "*.parquet"), bp_rel)
        _copy_partitioned(con, f"SELECT *, security_id % {nb} AS b FROM ({sql})", _tmp(IN_DIR, "panel", f"year={y}"),
                          "b")
        out[str(y)] = {"borrow_proxy": bp_rel is not None}
    hist = root / "prices_history"
    if list(hist.glob("year=*/*.parquet")):
        _copy_partitioned(con, f"SELECT session_date, security_id, ret, ret_guarded, security_id % {nb} AS b "
                               f"FROM {_rp(hist / 'year=*' / '*.parquet')} WHERE session_date < DATE '{PANEL_START}'",
                          _tmp(IN_DIR, "hist"), "b")
        out["hist"] = True
    else:
        out["hist"] = False
    daily = _mkt_daily_rel(nb)
    dest = _tmp(LINE_DIR, "mkt_month.parquet")
    out["mkt_month_rows"] = C.copy_to_parquet(con, mkt_month_sql(daily, C.calendar_path().as_posix()), dest)
    return out


def _mkt_daily_rel(nb: int) -> str:
    b = MKT_ID % nb
    parts = [f"SELECT session_date, security_id, ret, ret_guarded FROM {_rp(_tmp(IN_DIR, 'panel', 'year=*', f'b={b}', '*.parquet'))}"]
    if list(_tmp(IN_DIR, "hist").glob(f"b={b}/*.parquet")):
        parts.append(f"SELECT session_date, security_id, ret, ret_guarded FROM {_rp(_tmp(IN_DIR, 'hist', f'b={b}', '*.parquet'))}")
    return "(" + " UNION ALL ".join(parts) + ")"


def _line_columns(con, sql: str) -> list[str]:
    return [r[0] for r in con.execute(f"DESCRIBE SELECT * FROM ({sql})").fetchall()]


def line_bucket(con, b: int, years: Sequence[int], features: Sequence[R.Feature],
                extra_terms: dict[str, str] | None = None) -> dict[str, Any]:
    inp = _rp(_tmp(IN_DIR, "panel", "year=*", f"b={b}", "*.parquet"))
    hist_glob = _tmp(IN_DIR, "hist", f"b={b}")
    hist = _rp(hist_glob / "*.parquet") if list(hist_glob.glob("*.parquet")) else None
    mk = _tmp(LINE_DIR, "mkt_month.parquet")
    sql, keep = line_query(con, inp, hist, _rp(mk) if mk.exists() else None, C.calendar_path().as_posix(), features,
                           extra_terms)
    cols = ", ".join(f'"{c}"' for c in keep)
    dest = _tmp(LINE_DIR, f"b={b}")
    _copy_partitioned(con, f"SELECT {cols}, year(session_date) AS yr FROM ({sql}) "
                           f"WHERE session_date >= DATE '{min(years)}-01-01' AND session_date <= DATE '{max(years)}-12-31'",
                      dest, "yr")
    rows = con.execute(f"SELECT count(*) FROM {_rp(dest / 'yr=*' / '*.parquet')}").fetchone()[0]
    return {"rows": int(rows), "columns": len(keep)}


def year_pass(con, year: int, features: Sequence[R.Feature]) -> dict[str, Any]:
    src = _rp(_tmp(LINE_DIR, "b=*", f"yr={year}", "*.parquet"))
    dest = C.stage_dir(STAGE) / f"year={year}" / "characteristics.parquet"
    sql = year_sql(src, dt.date(year, 1, 1), dt.date(year, 12, 31), features) + " ORDER BY session_date, security_id"
    rows = C.copy_to_parquet(con, sql, dest, row_group_size=32768)
    return {"rows": rows, "bytes": dest.stat().st_size}


def coverage(con, features: Sequence[R.Feature], years: tuple[int, int] = COVERAGE_YEARS) -> dict[str, Any]:
    """Mean daily share of member_equity lines with a non-null value (fill_zero zeros count), per feature, over the
    sessions of ``years`` (inclusive), plus per calendar year."""
    glob = _rp(C.stage_dir(STAGE) / "year=*" / "*.parquet")
    names = [f.name for f in features]
    cnt = ", ".join(f'count("{n}") AS "{n}"' for n in names)
    rows = con.execute(f"""
        SELECT year(session_date) AS y, session_date, count(*) AS n, {cnt}
        FROM {glob} WHERE member_equity AND year(session_date) BETWEEN {years[0]} AND {years[1]}
        GROUP BY 1, 2 ORDER BY 2""").fetchall()
    out: dict[str, Any] = {"window": list(years), "sessions": len(rows), "features": {}}
    for j, n in enumerate(names):
        shares = [r[3 + j] / r[2] for r in rows if r[2]]
        per_year: dict[str, float] = {}
        for yv in sorted({r[0] for r in rows}):
            s = [r[3 + j] / r[2] for r in rows if r[0] == yv and r[2]]
            per_year[str(yv)] = round(sum(s) / len(s), 4) if s else None
        out["features"][n] = {"mean": round(sum(shares) / len(shares), 4) if shares else None, "per_year": per_year}
    return out


def run(stages: Sequence[str], years: Sequence[int], *, nb: int = NB, features: Sequence[R.Feature] | None = None,
        extra_terms: dict[str, str] | None = None, memory: str = "600MB", threads: int = 2) -> dict[str, Any]:
    feats = R.validate(features if features is not None else R.REGISTRY)
    problems = lint_build(feats)
    if problems:
        raise ValueError(f"look-ahead lint: {problems}")
    years = sorted(years)
    sig = _signature(nb, feats, extra_terms)
    prog_path = _tmp(LINE_DIR, "progress.json")
    prog = C.read_json(prog_path) if prog_path.exists() else {}
    if prog.get("signature") != sig:
        prog = {"signature": sig, "extract": None, "buckets": {}, "years": {}}
    receipt: dict[str, Any] = {"stage": STAGE, "nb": nb, "years": years}
    con = C.connect(memory=memory, threads=threads, db_file="characteristics.duckdb")
    try:
        if "extract" in stages:
            if prog["extract"] and prog["extract"].get("years") == years:
                receipt["extract"] = prog["extract"]
            else:
                with C.timed(receipt, "extract"):
                    receipt["extract"] = {"years": years, **extract(con, years, nb)}
                prog["extract"] = receipt["extract"]
                prog["buckets"], prog["years"] = {}, {}
                C.write_json_atomic(prog_path, prog)
        if "line" in stages:
            with C.timed(receipt, "line"):
                for b in range(nb):
                    if str(b) in prog["buckets"]:
                        continue
                    t0 = time.perf_counter()
                    res = line_bucket(con, b, years, feats, extra_terms)
                    res["elapsed_s"] = round(time.perf_counter() - t0, 1)
                    prog["buckets"][str(b)] = res
                    prog["years"] = {}
                    C.write_json_atomic(prog_path, prog)
                    print("bucket", b, res, flush=True)
            receipt["line"] = prog["buckets"]
        if "year" in stages:
            with C.timed(receipt, "year"):
                for y in years:
                    if str(y) in prog["years"]:
                        continue
                    t0 = time.perf_counter()
                    res = year_pass(con, y, feats)
                    res["elapsed_s"] = round(time.perf_counter() - t0, 1)
                    prog["years"][str(y)] = res
                    C.write_json_atomic(prog_path, prog)
                    print("year", y, res, flush=True)
            receipt["year"] = prog["years"]
        if "manifest" in stages:
            with C.timed(receipt, "coverage"):
                receipt["coverage"] = coverage(con, feats)
            payload = {"registry": R.as_manifest(feats), "receipt": receipt, "progress_signature": sig,
                       "input_manifests_sha256": _input_manifests(),
                       "rules": {"pit": "Global Constraint 4; iv_atm_* lag 1 (ruling R3); vendor earnFlag unused",
                                 "buckets": f"security_id % {nb}", "group_min": GROUP_MIN, "mkt_id": MKT_ID}}
            C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    finally:
        con.close()
    return receipt


# ------------------------------------------------------------------------------------------------ PIT harness

def _eq_sql(a: str, b: str) -> str:
    return (f"(({a} IS NULL AND {b} IS NULL) OR ({a} IS NOT NULL AND {b} IS NOT NULL AND "
            f"(abs({a} - {b}) <= {PIT_REL_TOL} * greatest(abs({a}), abs({b})) OR abs({a} - {b}) <= {PIT_ABS_TOL})))")


def pit_source(con, bucket: int, dmax: dt.date, *, nb: int = NB, table: str = "pit_src") -> str:
    """Materialize the bucket's line-pass input read from the PUBLISHED panel and borrow_proxy stages (not the
    build's extracts), rows ``session_date <= dmax`` only; ``pit_check(source=...)`` cuts it at each d."""
    root = C.build_root()
    cut = f"session_date <= DATE '{dmax}'"
    pan = f"(SELECT * FROM {_rp(root / 'panel' / 'year=*' / '*.parquet')} WHERE security_id % {nb} = {bucket} AND {cut})"
    bp = (f"(SELECT * FROM {_rp(root / 'borrow_proxy' / 'year=*' / '*.parquet')} "
          f"WHERE security_id % {nb} = {bucket} AND {cut})") if list((root / "borrow_proxy").glob("year=*/*.parquet")) else None
    con.execute(f"CREATE OR REPLACE TABLE {table} AS {input_sql(pan, bp)}")
    return table


def pit_check(con, bucket: int, d: dt.date, *, nb: int = NB, features: Sequence[R.Feature] | None = None,
              extra_terms: dict[str, str] | None = None, max_examples: int = 3,
              source: str | None = None) -> dict[str, Any]:
    """Recompute bucket ``bucket`` at decision session ``d`` from panel / borrow_proxy / prices_history rows with
    ``session_date <= d`` only (read from the published stages, not the build's extracts), then compare every feature
    with the built stage at d: equal (|diff| <= 1e-9 relative, or <= 1e-12) or both NULL.

    Group statistics at d combine the recomputed bucket with the build's line rows of the other buckets at d (same
    session cross section; those rows come from the same line SQL, checked here on the tested bucket)."""
    feats = list(features if features is not None else R.REGISTRY)
    root = C.build_root()
    cal = C.calendar_path().as_posix()
    cut = f"session_date <= DATE '{d}'"
    src = source or pit_source(con, bucket, d, nb=nb)
    hist_files = list((root / "prices_history").glob("year=*/*.parquet"))
    hist = (f"(SELECT * FROM {_rp(root / 'prices_history' / 'year=*' / '*.parquet')} "
            f"WHERE security_id % {nb} = {bucket} AND {cut})") if hist_files else None
    con.execute(f"CREATE OR REPLACE TEMP VIEW pit_in AS SELECT * FROM {src} WHERE {cut}")
    mk_parts = [f"SELECT session_date, security_id, ret, ret_guarded FROM {_rp(root / 'panel' / 'year=*' / '*.parquet')} "
                f"WHERE security_id = {MKT_ID} AND {cut}"]
    if hist_files:
        mk_parts.append(f"SELECT session_date, security_id, ret, ret_guarded FROM "
                        f"{_rp(root / 'prices_history' / 'year=*' / '*.parquet')} WHERE security_id = {MKT_ID} "
                        f"AND session_date < DATE '{PANEL_START}' AND {cut}")
    con.execute(f"CREATE OR REPLACE TEMP TABLE pit_mkt AS {mkt_month_sql('(' + ' UNION ALL '.join(mk_parts) + ')', cal)}")
    sql, keep = line_query(con, "pit_in", hist, "pit_mkt", cal, feats, extra_terms, tag="pit")
    cols = ", ".join(f'"{c}"' for c in keep)
    con.execute(f"CREATE OR REPLACE TEMP TABLE pit_line AS SELECT {cols} FROM ({sql}) WHERE session_date = DATE '{d}'")
    others = [p for p in _tmp(LINE_DIR).glob(f"b=*/yr={d.year}/*.parquet") if p.parent.parent.name != f"b={bucket}"]
    other_rel = ("SELECT " + cols + " FROM read_parquet([" + ", ".join(f"'{p.as_posix()}'" for p in others)
                 + f"], hive_partitioning = false, union_by_name = true) WHERE session_date = DATE '{d}'") if others else None
    union = f"(SELECT * FROM pit_line{(' UNION ALL ' + other_rel) if other_rel else ''})"
    con.execute(f"CREATE OR REPLACE TEMP TABLE pit_new AS SELECT * FROM ({year_sql(union, d, d, feats)}) "
                f"WHERE security_id % {nb} = {bucket}")
    built = _rp(C.stage_dir(STAGE) / f"year={d.year}" / "*.parquet")
    con.execute(f"CREATE OR REPLACE TEMP TABLE pit_old AS SELECT * FROM {built} "
                f"WHERE session_date = DATE '{d}' AND security_id % {nb} = {bucket}")
    n_new, n_old = (con.execute(f"SELECT count(*) FROM {t}").fetchone()[0] for t in ("pit_new", "pit_old"))
    n_both = con.execute("SELECT count(*) FROM pit_new JOIN pit_old USING (security_id)").fetchone()[0]
    res: dict[str, Any] = {"bucket": bucket, "session": str(d), "rows_recomputed": n_new, "rows_built": n_old,
                           "rows_matched": n_both, "mismatches": {}, "non_null_compared": {}}
    names = [f.name for f in feats]
    agg = ", ".join(f'count(*) FILTER (WHERE NOT {_eq_sql(f"n.{q}", f"o.{q}")}), '
                    f"count(*) FILTER (WHERE n.{q} IS NOT NULL AND o.{q} IS NOT NULL)"
                    for q in (f'"{x}"' for x in names))
    row = con.execute(f"SELECT {agg} FROM pit_new n JOIN pit_old o USING (security_id)").fetchone()
    for j, name in enumerate(names):
        bad, nn = row[2 * j], row[2 * j + 1]
        res["non_null_compared"][name] = nn
        if bad:
            a, b = f'n."{name}"', f'o."{name}"'
            ex = con.execute(f"SELECT security_id, {a}, {b} FROM pit_new n JOIN pit_old o USING (security_id) "
                             f"WHERE NOT {_eq_sql(a, b)} LIMIT {max_examples}").fetchall()
            res["mismatches"][name] = {"count": bad, "examples": [list(map(str, e)) for e in ex]}
    res["pass"] = not res["mismatches"] and n_new == n_old == n_both
    return res


def run_pit(sessions: Sequence[dt.date], buckets: Sequence[int], *, nb: int = NB, memory: str = "600MB",
            threads: int = 2, out: Path | None = None) -> dict[str, Any]:
    con = C.connect(memory=memory, threads=threads, db_file="characteristics_pit.duckdb")
    results = []
    try:
        for b in buckets:
            t0 = time.perf_counter()
            src = pit_source(con, b, max(sessions), nb=nb)
            print("pit source bucket", b, round(time.perf_counter() - t0, 1), "s", flush=True)
            for d in sessions:
                t0 = time.perf_counter()
                r = pit_check(con, b, d, nb=nb, source=src)
                r["elapsed_s"] = round(time.perf_counter() - t0, 1)
                results.append(r)
                print(json.dumps({k: r[k] for k in ("bucket", "session", "rows_recomputed", "rows_built", "pass",
                                                    "elapsed_s")}), {k: v["count"] for k, v in r["mismatches"].items()},
                      flush=True)
    finally:
        con.close()
    summary = {"schema": "atx.alpha-panel.characteristics-pit/v1", "pass": all(r["pass"] for r in results),
               "tolerance": {"relative": PIT_REL_TOL, "absolute": PIT_ABS_TOL}, "results": results,
               "stage_manifest_sha256": (C.sha256_file(C.stage_dir(STAGE) / "manifest.json")
                                         if (C.stage_dir(STAGE) / "manifest.json").exists() else None)}
    if out:
        C.write_json_atomic(out, summary)
    return summary


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--stages", default="extract,line,year,manifest")
    ap.add_argument("--years", default="2019-2026")
    ap.add_argument("--memory", default="600MB")
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--pit", default="", help="comma-separated decision sessions: run the PIT harness instead")
    ap.add_argument("--pit-buckets", default="3,10")
    ap.add_argument("--out", default="")
    args = ap.parse_args(argv)
    if args.pit:
        sessions = [dt.date.fromisoformat(s.strip()) for s in args.pit.split(",") if s.strip()]
        buckets = [int(s) for s in args.pit_buckets.split(",") if s.strip()]
        out = Path(args.out) if args.out else C.build_root() / "validation" / "characteristics_pit.json"
        res = run_pit(sessions, buckets, memory=args.memory, threads=args.threads, out=out)
        print("PIT", "PASS" if res["pass"] else "FAIL", out, flush=True)
        return 0 if res["pass"] else 1
    a, _, b = args.years.partition("-")
    run([s.strip() for s in args.stages.split(",") if s.strip()], list(range(int(a), int(b or a) + 1)),
        memory=args.memory, threads=args.threads)
    return 0


if __name__ == "__main__":
    sys.exit(main())
