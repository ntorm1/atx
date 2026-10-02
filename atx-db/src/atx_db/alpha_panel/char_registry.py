"""Characteristics v2 feature registry: one entry per feature of the ``characteristics/`` stage.

Contract (consumed by the build in ``characteristics.py``, the gold selection and the IC report):

``Feature`` fields

* ``name`` (str): output column, unique, ``[a-z][a-z0-9_]*``.
* ``family`` (str): one of ``FAMILIES``; ``control`` for the risk/size/liquidity controls.
* ``sql_or_callable`` (str): a DuckDB SQL expression evaluated per row in the year pass over the line-pass output
  columns (panel inputs and time-series terms of ``characteristics.py``) and the same-session group statistics
  (``g49_*`` FF49, ``g12_*`` FF12 means over ``member`` lines, NULL when fewer than 3). A callable is reserved for
  later families and rejected by the current build.
* ``prior_sign`` (int, +1 / -1): the orientation ALREADY APPLIED to the stored value relative to the natural
  quantity named in ``notes`` (-1 = the natural quantity was negated so that higher = predicted higher return, the
  literature prior). Controls are stored with their natural sign and carry +1.
* ``citation`` (str): the literature source of the prior (and of the definition).
* ``inputs`` (tuple[str, ...]): ``<stage>.<column>`` of every input (``panel``, ``prices_history``,
  ``borrow_proxy``, ``calendar``).
* ``lookback_sessions`` (int): the longest trailing window in sessions (0 = as-of values only).
* ``kind`` (str): ``signal`` (selected on IC) or ``control`` (always shipped, natural sign).
* ``fill_zero`` (bool): absence means "no event" and the stored value is 0 (not NULL) on every panel row.
* ``notes`` (str): definition, NULL rules and caveats.

Point in time (Global Constraint 4): a value at session d uses only panel rows of the same line with
``session_date <= d`` and same-session cross sections; every ``iv_atm_*`` input is the line's previous-session
value (lag 1). ``lint_sql`` rejects ``lead(``, ``FOLLOWING`` and unbounded-frame whole-history statistics in the
production SQL; ``characteristics.pit_check`` proves the property on data.
"""

from __future__ import annotations

import re
from collections.abc import Callable, Iterable
from dataclasses import asdict, dataclass
from typing import Any

SCHEMA = "atx.alpha-panel.char-registry/v2"
KINDS = ("signal", "control")
# part A market families, the ported fundamental families, and the part B families (Task 5)
FAMILIES = (
    "momentum", "reversal", "low_risk", "liquidity", "volume", "options", "short_side", "ownership", "seasonality",
    "event_time", "control",
    "value", "profitability", "investment", "earnings_momentum", "issuance", "leverage", "intangibles",
    "distress", "insider", "events",
)
_NAME = re.compile(r"^[a-z][a-z0-9_]*$")
# forbidden constructs: future rows, and whole-partition statistics (a window with no ORDER BY / no frame end)
_FORBIDDEN = (
    (re.compile(r"\blead\s*\(", re.I), "lead("),
    (re.compile(r"\bFOLLOWING\b", re.I), "FOLLOWING"),
    (re.compile(r"\blag\s*\([^,()]+,\s*-", re.I), "negative lag"),
    (re.compile(r"OVER\s*\(\s*PARTITION\s+BY\s+(?:(?!\bORDER\b)[\w.,\s])+\)", re.I),
     "unordered window (whole-history statistic)"),
    (re.compile(r"OVER\s*\(\s*\)", re.I), "empty window (whole-sample statistic)"),
)


@dataclass(frozen=True)
class Feature:
    name: str
    family: str
    sql_or_callable: str | Callable[..., Any]
    prior_sign: int
    citation: str
    inputs: tuple[str, ...]
    lookback_sessions: int
    kind: str = "signal"
    fill_zero: bool = False
    notes: str = ""

    def as_dict(self) -> dict[str, Any]:
        d = asdict(self)
        d["inputs"] = list(self.inputs)
        if callable(self.sql_or_callable):
            d["sql_or_callable"] = f"<callable {getattr(self.sql_or_callable, '__name__', '?')}>"
        return d


def _f(name: str, family: str, sql: str, sign: int, citation: str, inputs: str, lookback: int, *,
       kind: str = "signal", fill_zero: bool = False, notes: str = "") -> Feature:
    return Feature(name, family, sql, sign, citation, tuple(s.strip() for s in inputs.split(",") if s.strip()),
                   lookback, kind, fill_zero, notes)


_R = "panel.ret, panel.ret_guarded"
_FUND = "panel fundamentals (clock < 22:00 UTC of d-1, fund-events-pit-v2)"

REGISTRY: tuple[Feature, ...] = (
    # ------------------------------------------------------------------ momentum
    _f("mom_12_1", "momentum", "mom_12_1", +1, "Jegadeesh & Titman (1993, JF); Carhart (1997, JF)",
       "panel.close", 252, notes="close[t-21]/close[t-252] - 1; NULL unless the line traded every session t-252..t"),
    _f("mom_6_1", "momentum", "mom_6_1", +1, "Jegadeesh & Titman (1993, JF), 6-month formation",
       "panel.close", 126, notes="close[t-21]/close[t-126] - 1; every session t-126..t present"),
    _f("mom_12_7", "momentum", "mom_12_7", +1, "Novy-Marx (2012, JFE) intermediate momentum",
       "panel.close", 252, notes="close[t-147]/close[t-252] - 1 (months t-12..t-7); every session present"),
    _f("tsmom_12", "momentum", "CASE WHEN vol_63 > 0 THEN sign(ret_252) / (vol_63 * sqrt(252)) END", +1,
       "Moskowitz, Ooi & Pedersen (2012, JFE) time-series momentum",
       f"panel.close, {_R}", 252,
       notes="sign of the 12-month return close[t]/close[t-252]-1 over annualized 63-session volatility"),
    _f("frog_in_pan", "momentum", "-fip", -1, "Da, Gurun & Warachka (2014, RFS) frog in the pan",
       f"panel.close, {_R}", 252,
       notes="natural ID = sign(PRET) * (%neg - %pos) over daily returns t-251..t-21 (PRET = mom_12_1); stored -ID "
             "(continuous information = stronger continuation); >= 200 returns, every session present"),
    _f("mom_overnight_12", "momentum", "ovn_12", +1, "Lou, Polk & Skouras (2019, JFE) overnight momentum",
       "panel.ret_overnight", 252, notes="sum of ret_overnight over t-251..t-21; >= 200 values, every session present"),
    _f("high_52w", "momentum", "close / nullif(max_close_252, 0)", +1, "George & Hwang (2004, JF)",
       "panel.close", 252, notes="close over its 252-session max (adjusted closes; ratio within the window)"),
    _f("ind_mom_12_1", "momentum", "g49_mom_12_1", +1, "Moskowitz & Grinblatt (1999, JF)",
       "panel.close, panel.grp_ff49, panel.member", 252,
       notes="same-session equal-weight mean of mom_12_1 over member lines of the FF49 group (>= 3)"),
    _f("ind_mom_6_1", "momentum", "g49_mom_6_1", +1, "Moskowitz & Grinblatt (1999, JF), 6-month formation",
       "panel.close, panel.grp_ff49, panel.member", 126,
       notes="same-session equal-weight mean of mom_6_1 over member lines of the FF49 group (>= 3)"),
    _f("sector_mom_ff12_12_1", "momentum", "g12_mom_12_1", +1, "Moskowitz & Grinblatt (1999, JF), FF12 sectors",
       "panel.close, panel.grp_ff12, panel.member", 252,
       notes="same-session equal-weight mean of mom_12_1 over member lines of the FF12 sector (>= 3)"),
    _f("within_ind_mom", "momentum", "mom_12_1 - g49_mom_12_1", +1,
       "Moskowitz & Grinblatt (1999, JF) intra-industry momentum", "panel.close, panel.grp_ff49, panel.member", 252,
       notes="mom_12_1 minus its FF49 same-session mean"),
    _f("resid_mom_12_1", "momentum", "resid_mom_12_1", +1, "Blitz, Huij & Martens (2011, JEF) residual momentum",
       f"{_R}, panel.mkt_ret", 252,
       notes="sum of market-model residuals r - beta_252*m over t-251..t-21 over their stdev; >= 200 residuals"),
    # ------------------------------------------------------------------ reversal
    _f("rev_5", "reversal", "-ret_5", -1, "Lehmann (1990, QJE); Lo & MacKinlay (1990, RFS)",
       "panel.close", 5, notes="natural = close[t]/close[t-5] - 1; every session present"),
    _f("rev_21", "reversal", "-ret_21", -1, "Jegadeesh (1990, JF)", "panel.close", 21,
       notes="natural = close[t]/close[t-21] - 1; every session present"),
    _f("intraday_rev_5", "reversal", "-ri_sum_5", -1, "Lou, Polk & Skouras (2019, JFE) intraday reversal",
       "panel.ret_intraday, panel.ret_guarded", 5,
       notes="natural = 5-session sum of ret_intraday (NULL on guarded rows); all 5 present"),
    _f("ind_adj_rev_5", "reversal", "-(ret_5 - g49_ret_5)", -1, "Da, Liu & Schaumburg (2014, MS)",
       "panel.close, panel.grp_ff49, panel.member", 5, notes="natural = ret_5 minus its FF49 same-session mean"),
    _f("ind_adj_rev_21", "reversal", "-(ret_21 - g49_ret_21)", -1, "Da, Liu & Schaumburg (2014, MS)",
       "panel.close, panel.grp_ff49, panel.member", 21, notes="natural = ret_21 minus its FF49 same-session mean"),
    _f("overnight_intraday_tug_21", "reversal", "ro_sum_21 - ri_sum_21", +1,
       "Lou, Polk & Skouras (2019, JFE) tug of war",
       "panel.ret_overnight, panel.ret_intraday, panel.ret_guarded", 21,
       notes="21-session sum of ret_overnight (prior +) minus 21-session sum of ret_intraday (prior -); all 21 present"),
    # ------------------------------------------------------------------ low risk
    _f("low_beta", "low_risk", "-beta_252", -1, "Frazzini & Pedersen (2014, JFE); Black, Jensen & Scholes (1972)",
       f"{_R}, panel.mkt_ret", 252, notes="natural = beta_252 (control)"),
    _f("low_ivol", "low_risk", "-ivol_21", -1, "Ang, Hodrick, Xing & Zhang (2006, JF)", f"{_R}, panel.mkt_ret", 21,
       notes="natural = ivol_21 = sd(r) * sqrt(1 - corr(r, m)^2) over 21 sessions"),
    _f("low_ivol_63", "low_risk", "-ivol_63", -1, "Ang, Hodrick, Xing & Zhang (2006, JF)", f"{_R}, panel.mkt_ret", 63,
       notes="natural = sd(r) * sqrt(1 - corr(r, m)^2) over 63 sessions (>= 60 returns)"),
    _f("low_max", "low_risk", "-max_21", -1, "Bali, Cakici & Whitelaw (2011, JFE)", _R, 21,
       notes="natural = max daily return over 21 sessions"),
    _f("lowvol_ind", "low_risk", "-vol_252", -1, "Blitz & van Vliet (2007, JPM); Baker, Bradley & Wurgler (2011, FAJ)",
       _R, 252, notes="natural = vol_252 (>= 240 returns)"),
    _f("low_vol_126", "low_risk", "-vol_126", -1, "Blitz & van Vliet (2007, JPM); consumer wish list vol_126",
       _R, 126, notes="natural = sd of daily returns over 126 sessions (>= 120)"),
    _f("low_skew_63", "low_risk", "-skew_63", -1,
       "Bali, Engle & Murray (2016, Empirical Asset Pricing); Amaya, Christoffersen, Jacobs & Vasquez (2015, JFE)",
       _R, 63, notes="natural = sample skewness of daily returns over 63 sessions (>= 60)"),
    _f("low_coskew_252", "low_risk", "-coskew_252", -1, "Harvey & Siddique (2000, JF)", f"{_R}, panel.mkt_ret", 252,
       notes="natural = E[e_i x^2] / (sqrt(E[e_i^2]) E[x^2]), x = demeaned mkt_ret, e_i = market-model residual, "
             "daily over 252 sessions (>= 200 pairs)"),
    _f("low_coskew_60m", "low_risk", "-coskew_60m", -1, "Harvey & Siddique (2000, JF); consumer wish list coskew_60m",
       "prices_history.ret, panel.ret, panel.ret_guarded, SPY line 549535 ret", 1260,
       notes="natural = Harvey-Siddique coskewness on monthly returns of the 60 calendar months ending with the last "
             "month completed at d (>= 48 complete months; a month counts when the line has an unguarded return on "
             "every calendar session); market = SPY (security_id 549535) monthly return; raw (not excess) returns; "
             "2012-2017 from prices_history"),
    _f("downside_beta_252", "low_risk", "-dbeta_252", -1,
       "Ang, Chen & Xing (2006, RFS) downside beta; sign per the low-risk prior (brief)", f"{_R}, panel.mkt_ret", 252,
       notes="natural = OLS slope of r on m over the days of the last 252 sessions with m < 0 (>= 50 days)"),
    # ------------------------------------------------------------------ liquidity / volume
    _f("amihud_21", "liquidity", "amihud_21", +1, "Amihud (2002, JFM)", f"{_R}, panel.dollar_volume", 21,
       notes="mean |r| per $1M traded over 21 sessions (every session present)"),
    _f("zero_return_days_63", "liquidity", "zero_63", +1, "Lesmond, Ogden & Trzcinka (1999, RFS)", _R, 63,
       notes="share of zero daily returns over 63 sessions (>= 60 returns)"),
    _f("vol_of_volume_63", "liquidity", "-cv_dv_63", -1, "Chordia, Subrahmanyam & Anshuman (2001, JFE)",
       "panel.dollar_volume", 63, notes="natural = CV (sd/mean) of dollar volume over 63 sessions (>= 60)"),
    _f("abn_turnover", "volume", "abn_turnover", +1, "Gervais, Kaniel & Mingelgrin (2001, JF)", "panel.volume", 252,
       notes="21-session over 252-session mean share volume, minus 1"),
    _f("abn_volume_5", "volume", "abn_vol_5", +1, "Gervais, Kaniel & Mingelgrin (2001, JF), 5-session",
       "panel.volume", 55, notes="mean volume t-4..t over mean volume t-54..t-5, minus 1; every session present"),
    _f("volume_trend_21_252", "volume", "-vtrend_252", -1, "Haugen & Baker (1996, JFE) trading-volume trend",
       "panel.dollar_volume", 252,
       notes="natural = OLS slope of ln(dollar volume) on the session index over 252 sessions x 21 (log growth per "
             "month; >= 200 obs)"),
    # ------------------------------------------------------------------ options (IV lag 1: Global Constraint 4)
    _f("iv_term_slope", "options", "iv252_l1 - iv21_l1", +1, "Vasquez (2017, JFQA)",
       "panel.iv_atm_252d, panel.iv_atm_21d", 1, notes="lag-1 iv_atm_252d minus lag-1 iv_atm_21d"),
    _f("iv_change_21", "options", "-iv63_chg21", -1, "An, Ang, Bali & Cakici (2014, JF)", "panel.iv_atm_63d", 22,
       notes="natural = iv_atm_63d[t-1] - iv_atm_63d[t-22]; every session present"),
    _f("iv_change_5", "options", "-iv21_chg5", -1, "An, Ang, Bali & Cakici (2014, JF)", "panel.iv_atm_21d", 6,
       notes="natural = iv_atm_21d[t-1] - iv_atm_21d[t-6]; every session present"),
    _f("iv_level_21", "options", "-iv21_bf", -1,
       "Ang, Hodrick, Xing & Zhang (2006, JF); An, Ang, Bali & Cakici (2014, JF)", "panel.iv_atm_21d", 6,
       notes="natural = iv_atm_21d of the latest line row t-6..t-1 with a value"),
    _f("iv_rv_spread", "options", "iv21_bf - vol_21 * sqrt(252)", +1, "Bali & Hovakimian (2009, MS)",
       f"panel.iv_atm_21d, {_R}", 21, notes="lagged ATM IV (t-6..t-1 back-fill) minus annualized vol_21"),
    _f("iv_rv_ratio", "options", "CASE WHEN vol_21 > 0 THEN iv21_bf / (vol_21 * sqrt(252)) END", +1,
       "Bali & Hovakimian (2009, MS)", f"panel.iv_atm_21d, {_R}", 21,
       notes="lagged ATM IV over annualized vol_21"),
    # ------------------------------------------------------------------ short side
    _f("si_ratio", "short_side", "-si_ratio_raw", -1, "Asquith, Pathak & Ritter (2005, JFE); Dechow et al. (2001, JFE)",
       "panel.si_shares, panel.shares_out", 0, notes="natural = si_shares / shares_out"),
    _f("si_change", "short_side", "-(si_ratio_raw - si_ratio_21)", -1, "Boehmer, Huszar & Jordan (2010, JFE)",
       "panel.si_shares, panel.shares_out", 21, notes="natural = si ratio now minus 21 sessions ago"),
    _f("dtc", "short_side", "-si_dtc", -1, "Hong, Li, Ni, Scheinkman & Yan (2015)", "panel.si_dtc", 0,
       notes="natural = FINRA days to cover"),
    _f("si_to_adv", "short_side", "CASE WHEN vol_avg_21 > 0 THEN -(si_shares / vol_avg_21) END", -1,
       "Hong, Li, Ni, Scheinkman & Yan (2015)", "panel.si_shares, panel.volume", 21,
       notes="natural = si_shares over 21-session mean volume (unfloored days to cover)"),
    _f("short_vol_ratio_5", "short_side", "-svr_5", -1, "Diether, Lee & Werner (2009, RFS)",
       "panel.sv_short_volume, panel.sv_total_volume", 5, notes="natural = mean daily short-volume share (>= 4 of 5)"),
    _f("short_vol_ratio_21", "short_side", "-svr_21", -1, "Diether, Lee & Werner (2009, RFS)",
       "panel.sv_short_volume, panel.sv_total_volume", 21, notes="natural = mean daily short-volume share (>= 15 of 21)"),
    _f("ftd_to_shares", "short_side", "CASE WHEN shares_out > 0 THEN -(ftd_quantity / shares_out) END", -1,
       "Evans, Geczy, Musto & Reed (2009, RFS); Boni (2006, JFM)", "panel.ftd_quantity, panel.shares_out", 0,
       notes="natural = panel ftd_quantity (latest settlement row of the latest visible SEC FTD file, stale 30 d; "
             "absence is NULL) over shares_out"),
    _f("ftd_latest_to_shares", "short_side", "CASE WHEN shares_out > 0 THEN -(bp_ftd_quantity / shares_out) END", -1,
       "Evans, Geczy, Musto & Reed (2009, RFS); Boni (2006, JFM)", "borrow_proxy.ftd_quantity, panel.shares_out", 0,
       notes="natural = fails on the latest published settlement date (borrow_proxy: 0 when absent from that file, "
             "NULL when stale > 60 d) over shares_out"),
    _f("regsho_threshold", "short_side", "CASE WHEN bp_on_threshold THEN -1.0 ELSE 0.0 END", -1,
       "Boni (2006, JFM); Evans, Geczy, Musto & Reed (2009, RFS)", "borrow_proxy.on_threshold_list", 0,
       fill_zero=True, notes="natural = 1 when on any market's latest visible Reg SHO threshold list at d"),
    _f("regsho_run_days", "short_side",
       "CASE WHEN bp_on_threshold AND bp_run_days > 0 THEN -ln(1 + bp_run_days) ELSE 0.0 END", -1,
       "Boni (2006, JFM); Evans, Geczy, Musto & Reed (2009, RFS)", "borrow_proxy.threshold_run_days", 0,
       fill_zero=True, notes="natural = ln(1 + consecutive threshold-list days) when on a list, else 0"),
    _f("si_to_io", "short_side", "-bp_si_to_io", -1, "Asquith, Pathak & Ritter (2005, JFE); Nagel (2005, JFE)",
       "borrow_proxy.si_to_io", 0,
       notes="natural = short interest over 13F institutional shares (borrow_proxy clock, both within staleness)"),
    # ------------------------------------------------------------------ ownership (13F, panel inst_* clock)
    _f("io_ratio", "ownership", "CASE WHEN shares_out > 0 THEN inst_shares / shares_out END", +1,
       "Gompers & Metrick (2001, QJE)", "panel.inst_shares, panel.shares_out", 0,
       notes="13F institutional shares (as-of-45-days aggregate) over shares_out"),
    _f("io_change_q", "ownership", "inst_pct_change", +1, "Nofsinger & Sias (1999, JF); Sias, Starks & Titman (2006)",
       "panel.inst_pct_change", 0, notes="quarter-on-quarter relative change of 13F institutional shares"),
    _f("breadth_change", "ownership",
       "CASE WHEN inst_n_holders - inst_d_holders > 0 THEN CAST(inst_d_holders AS DOUBLE) / "
       "(inst_n_holders - inst_d_holders) END", +1, "Chen, Hong & Stein (2002, JFE)",
       "panel.inst_d_holders, panel.inst_n_holders", 0, notes="change in 13F holders over lagged holders"),
    _f("io_concentration", "ownership", "-inst_top10_share", -1, "Chen, Hong & Stein (2002, JFE) breadth",
       "panel.inst_top10_share", 0, notes="natural = share of 13F shares held by the top 10 holders"),
    # ------------------------------------------------------------------ seasonality
    _f("seasonality_same_month", "seasonality", "seas_same_month", +1, "Heston & Sadka (2008, JFE)",
       "panel.close", 252, notes="close[t-224]/close[t-245] - 1: the year-ago window of the next month; "
                                 "every session present (v1 name kept; brief alias seas_same_month)"),
    _f("seas_annual_avg_2_5", "seasonality", "seas_2_5", +1,
       "Heston & Sadka (2008, JFE); Keloharju, Linnainmaa & Nyberg (2016, JF)",
       "prices_history.ret, panel.ret, panel.ret_guarded", 1252,
       notes="mean over years k=2..5 available at d of the return over sessions [t-252k+8, t-252k+28] (21 unguarded "
             "returns required per year, calendar-session RANGE frames); >= 1 year; 2012-2017 from prices_history"),
    # ------------------------------------------------------------------ event time
    _f("ea_window_ahead_5", "event_time", "CASE WHEN ea_gap BETWEEN 2 AND 6 THEN 1.0 ELSE 0.0 END", +1,
       "Frazzini & Lamont (2007); Barber, De George, Lehavy & Trueman (2013, JFE)",
       "panel.earn_next_expected_date, panel.earn_day_offset, calendar", 300, fill_zero=True,
       notes="1 when the next expected announcement falls in sessions d+2..d+6. next expected = the smallest "
             "earn_next_expected_date (+364 d of a visible SEC 8-K 2.02 primary announcement) seen on the line's rows "
             "t-300..t that is after d and more than 30 days after the last reaction session; the current value alone "
             "is always 252-363 days ahead (it projects the SAME quarter next year)"),
    _f("earn_days_since", "event_time", "earn_days_since", +1, "event-time control",
       "panel.earn_day_offset", 0, kind="control",
       notes="sessions since the last SEC 8-K 2.02 reaction session (earn_day_offset = 0, known by 22:00 UTC of it); "
             "v1 used the vendor earnFlag '0' (not point in time: its '-1' precedes '0')"),
    # ------------------------------------------------------------------ controls (natural sign)
    _f("log_me", "control", "CASE WHEN me_company > 0 THEN ln(me_company) END", +1, "Banz (1981, JFE) size",
       "panel.me_company", 0, kind="control"),
    _f("log_me_line", "control", "CASE WHEN me_line > 0 THEN ln(me_line) END", +1, "size (line)",
       "panel.me_line", 0, kind="control"),
    _f("beta_252", "control", "beta_252", +1, "Frazzini & Pedersen (2014, JFE) beta estimator",
       f"{_R}, panel.mkt_ret", 252, kind="control",
       notes="corr of 3-day summed r and m over 250 sessions x vol_252 / sd(m) over 252"),
    _f("vol_21", "control", "vol_21", +1, "volatility control", _R, 21, kind="control", notes="all 21 returns"),
    _f("vol_63", "control", "vol_63", +1, "volatility control", _R, 63, kind="control", notes=">= 60 returns"),
    _f("vol_252", "control", "vol_252", +1, "volatility control", _R, 252, kind="control", notes=">= 240 returns"),
    _f("ivol_21", "control", "ivol_21", +1, "Ang, Hodrick, Xing & Zhang (2006, JF)", f"{_R}, panel.mkt_ret", 21,
       kind="control"),
    _f("turnover_21", "control", "CASE WHEN shares_out > 0 THEN vol_avg_21 / shares_out END", +1,
       "Datar, Naik & Radcliffe (1998, JFM)", "panel.volume, panel.shares_out", 21, kind="control"),
    _f("hl_spread_21", "control", "hl_spread_21", +1, "Corwin & Schultz (2012, JF)", "panel.high, panel.low", 21,
       kind="control", notes="mean two-day high-low spread estimate (floored at 0) over 21 sessions (cost input)"),
    _f("log_price", "control", "CASE WHEN raw_close > 0 THEN ln(raw_close) END", +1, "price-level control",
       "panel.raw_close", 0, kind="control"),
    _f("log_adv63", "control", "CASE WHEN adv63 > 0 THEN ln(adv63) END", +1, "liquidity control",
       "panel.adv63", 63, kind="control", notes="ln of the panel's prior 63-session mean dollar volume"),
    # ------------------------------------------------------------------ ported fundamentals (part B extends)
    _f("bm", "value", "CASE WHEN me_company > 0 AND be > 0 THEN be / me_company END", +1,
       "Fama & French (1992, JF); Rosenberg, Reid & Lanstein (1985, JPM)", _FUND, 0),
    _f("ep", "value", "CASE WHEN me_company > 0 AND ni_ttm > 0 THEN ni_ttm / me_company END", +1, "Basu (1977, JF)",
       _FUND, 0),
    _f("cfp", "value", "CASE WHEN me_company > 0 THEN cfo_ttm / me_company END", +1,
       "Lakonishok, Shleifer & Vishny (1994, JF)", _FUND, 0),
    _f("sp", "value", "CASE WHEN me_company > 0 AND sale_ttm > 0 THEN sale_ttm / me_company END", +1,
       "Barbee, Mukherji & Raines (1996, FAJ)", _FUND, 0),
    _f("ebit_ev", "value",
       "CASE WHEN me_company + coalesce(debt, 0) - coalesce(che, 0) > 0 AND oi_ttm > 0 "
       "THEN oi_ttm / (me_company + coalesce(debt, 0) - coalesce(che, 0)) END", +1,
       "Loughran & Wellman (2011, JFQA)", _FUND, 0),
    _f("fcfp", "value", "CASE WHEN me_company > 0 THEN (cfo_ttm - capx_ttm) / me_company END", +1,
       "Lakonishok, Shleifer & Vishny (1994, JF)", _FUND, 0),
    _f("dvc_yield", "value", "CASE WHEN me_company > 0 THEN dvc_ttm / me_company END", +1,
       "Litzenberger & Ramaswamy (1979, JFE)", _FUND, 0),
    _f("net_payout", "value", "CASE WHEN me_company > 0 THEN (dvc_ttm + prstkc_ttm - sstk_ttm) / me_company END", +1,
       "Boudoukh, Michaely, Richardson & Roberts (2007, JF)", _FUND, 0),
    _f("gpa", "profitability", 'CASE WHEN "at" > 0 THEN gp_ttm / "at" END', +1, "Novy-Marx (2013, JFE)", _FUND, 0),
    _f("opbe", "profitability", "CASE WHEN be > 0 THEN oi_ttm / be END", +1, "Fama & French (2015, JFE)", _FUND, 0),
    _f("opex_at", "profitability", 'CASE WHEN "at" > 0 THEN (sale_ttm - oi_ttm) / "at" END', +1,
       "Novy-Marx (2011, RF) operating leverage", _FUND, 0),
    _f("roa", "profitability", 'CASE WHEN "at" > 0 THEN ni_ttm / "at" END', +1,
       "Balakrishnan, Bartov & Faurel (2010, JAE)", _FUND, 0),
    _f("roe_q", "profitability", "CASE WHEN be_lag1q > 0 THEN ni_q / be_lag1q END", +1,
       "Hou, Xue & Zhang (2015, RFS)", _FUND, 0),
    _f("cfoa", "profitability", 'CASE WHEN "at" > 0 AND at_lag4 > 0 THEN cfo_ttm / (("at" + at_lag4) / 2) END', +1,
       "Asness, Frazzini & Pedersen (2019, RAS) QMJ", _FUND, 0),
    _f("fscore", "profitability", "fscore", +1, "Piotroski (2000, JAR)", _FUND, 0),
    _f("accruals", "profitability",
       'CASE WHEN "at" > 0 AND at_lag4 > 0 THEN -((ni_ttm - cfo_ttm) / (("at" + at_lag4) / 2)) END', -1,
       "Sloan (1996, AR)", _FUND, 0, notes="natural = (ni_ttm - cfo_ttm) / mean assets"),
    _f("asset_growth", "investment", 'CASE WHEN at_lag4 > 0 THEN -("at" / at_lag4 - 1) END', -1,
       "Cooper, Gulen & Schill (2008, JF)", _FUND, 0, notes="natural = at / at_lag4 - 1"),
    _f("noa_at", "investment", "CASE WHEN at_lag4 > 0 THEN -(noa / at_lag4) END", -1,
       "Hirshleifer, Hou, Teoh & Zhang (2004, JAE)", _FUND, 0, notes="natural = noa / at_lag4"),
    _f("capx_at", "investment", 'CASE WHEN "at" > 0 THEN -(capx_ttm / "at") END', -1,
       "Titman, Wei & Xie (2004, JFQA)", _FUND, 0, notes="natural = capx_ttm / at"),
    _f("sale_growth_q", "investment", "CASE WHEN sale_q_lag4 > 0 THEN -(sale_q / sale_q_lag4 - 1) END", -1,
       "Lakonishok, Shleifer & Vishny (1994, JF)", _FUND, 0,
       notes="natural = sale_q / sale_q_lag4 - 1; v1 stored it raw, v2 applies the prior sign"),
    _f("sue", "earnings_momentum", "sue", +1, "Foster, Olsen & Shevlin (1984, AR); Bernard & Thomas (1989, JAR)",
       _FUND, 0),
    _f("ear", "earnings_momentum", "ear_raw", +1, "Chan, Jegadeesh & Lakonishok (1996, JF); Brandt et al. (2008)",
       f"{_R}, panel.mkt_ret, panel.earn_recent", 126,
       notes="r - m summed over the 3 sessions ending the session after the SEC reaction session (earn_recent = 1 "
             "there and the session before), carried up to 125 sessions"),
    _f("droe", "earnings_momentum",
       "CASE WHEN be_lag1q > 0 AND be_lag1q_lag4 > 0 THEN ni_q / be_lag1q - ni_q_lag4 / be_lag1q_lag4 END", +1,
       "Hou, Xue & Zhang (2020, RFS)", _FUND, 0),
    _f("chtax", "earnings_momentum", "(txt_q - txt_q_lag4) / nullif(at_lag4, 0)", +1, "Thomas & Zhang (2011, JAR)",
       _FUND, 0),
    _f("gm_change", "earnings_momentum", "gm_change", +1, "Abarbanell & Bushee (1998, AR)", _FUND, 252,
       notes="TTM gross margin now minus 252 line rows ago"),
    _f("issuance_xbrl", "issuance", "CASE WHEN shrs_q > 0 AND shrs_q_lag4 > 0 THEN -ln(shrs_q / shrs_q_lag4) END", -1,
       "Pontiff & Woodgate (2008, JF)", _FUND, 0, notes="natural = ln share growth, filed shares"),
    _f("issuance_vendor", "issuance",
       "CASE WHEN adj_shares > 0 AND adj_shares_252 > 0 THEN -ln(adj_shares / adj_shares_252) END", -1,
       "Pontiff & Woodgate (2008, JF)", "panel.shares_out, panel.raw_close, panel.close", 252,
       notes="natural = ln growth of split-adjusted shares_out over 252 sessions"),
    _f("leverage", "leverage", 'CASE WHEN "at" > 0 THEN debt / "at" END', +1, "Bhandari (1988, JF)", _FUND, 0,
       notes="prior ambiguous (distress literature is negative); part B may re-kind"),
    _f("cash_at", "leverage", 'CASE WHEN "at" > 0 THEN che / "at" END', +1, "Palazzo (2012, JFE)", _FUND, 0),
    _f("rd_me", "intangibles", "CASE WHEN me_company > 0 AND xrd_ttm > 0 THEN xrd_ttm / me_company END", +1,
       "Chan, Lakonishok & Sougiannis (2001, JF)", _FUND, 0),
)

# v1 names kept for evaluate.py (library v5.1 scorecard base quantities, then the v1 additional set)
SCORECARD_NAMES = (
    "chtax", "droe", "ear", "sue", "asset_growth", "noa_at", "issuance_xbrl", "issuance_vendor", "low_beta", "low_ivol",
    "low_max", "lowvol_ind", "iv_rv_spread", "high_52w", "mom_12_1", "ind_mom_12_1", "within_ind_mom", "accruals",
    "cfoa", "fscore", "gpa", "opbe", "opex_at", "roa", "roe_q", "ind_adj_rev_5", "seasonality_same_month", "dtc",
    "si_change", "si_ratio", "bm", "cfp", "ebit_ev", "ep", "fcfp", "net_payout", "rd_me", "sp",
)
EXTRA_NAMES = (
    "resid_mom_12_1", "rev_21", "ind_adj_rev_21", "iv_term_slope", "iv_change_21", "iv_rv_ratio", "short_vol_ratio_5",
    "short_vol_ratio_21", "abn_turnover", "amihud_21", "hl_spread_21", "beta_252", "vol_21", "vol_252", "ivol_21",
    "turnover_21", "log_me", "log_me_line", "sale_growth_q", "gm_change", "leverage", "cash_at", "capx_at", "dvc_yield",
    "earn_days_since", "si_to_adv",
)


def lint_sql(sql: str) -> list[str]:
    """Forbidden look-ahead constructs found in ``sql`` (empty = clean)."""
    return [label for pat, label in _FORBIDDEN if pat.search(sql)]


def validate(features: Iterable[Feature] = REGISTRY) -> list[Feature]:
    """Check the registry contract; return the list (raises ``ValueError`` on the first violation)."""
    out = list(features)
    seen: set[str] = set()
    for f in out:
        where = f"feature {f.name!r}"
        if not _NAME.match(f.name) or f.name in seen:
            raise ValueError(f"{where}: bad or duplicate name")
        seen.add(f.name)
        if f.family not in FAMILIES:
            raise ValueError(f"{where}: family {f.family!r} not in FAMILIES")
        if f.kind not in KINDS:
            raise ValueError(f"{where}: kind {f.kind!r}")
        if f.prior_sign not in (1, -1):
            raise ValueError(f"{where}: prior_sign must be +1 or -1")
        if f.kind == "control" and f.prior_sign != 1:
            raise ValueError(f"{where}: controls keep their natural sign (prior_sign +1)")
        if f.family == "control" and f.kind != "control":
            raise ValueError(f"{where}: family control requires kind control")
        if not isinstance(f.fill_zero, bool) or not isinstance(f.lookback_sessions, int) or f.lookback_sessions < 0:
            raise ValueError(f"{where}: fill_zero must be bool, lookback_sessions a non-negative int")
        if not f.citation or not f.inputs:
            raise ValueError(f"{where}: citation and inputs are required")
        if isinstance(f.sql_or_callable, str):
            bad = lint_sql(f.sql_or_callable)
            if bad or not f.sql_or_callable.strip():
                raise ValueError(f"{where}: forbidden SQL {bad}")
        elif not callable(f.sql_or_callable):
            raise ValueError(f"{where}: sql_or_callable must be SQL text or a callable")
    return out


def names(kind: str | None = None, family: str | None = None) -> list[str]:
    return [f.name for f in REGISTRY if (kind is None or f.kind == kind) and (family is None or f.family == family)]


def by_name() -> dict[str, Feature]:
    return {f.name: f for f in REGISTRY}


def family_counts(features: Iterable[Feature] = REGISTRY) -> dict[str, int]:
    out: dict[str, int] = {}
    for f in features:
        out[f.family] = out.get(f.family, 0) + 1
    return dict(sorted(out.items()))


def as_manifest(features: Iterable[Feature] = REGISTRY) -> dict[str, Any]:
    feats = list(features)
    return {"schema": SCHEMA, "n_features": len(feats), "families": family_counts(feats),
            "features": [f.as_dict() for f in feats]}
