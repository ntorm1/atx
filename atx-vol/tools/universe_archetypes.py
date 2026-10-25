#!/usr/bin/env python3
"""Label-free universe archetypes for the lane-F fit robustness benchmark (wave 3, 2026-10-24).

Seven new per-underlier predicates over quote features (the §2.4 archetypes of
.superpowers/sdd/2026-10-24-risk-engine/wave3-fit-robustness-design.md that tools/shape_cohort.py
does not already define), plus two committed ticker rosters (the store types no underlier as an
ETP or an ADR). Like shape_cohort.py, every rule is ONE Python expression evaluated with no
builtins over the feature names below, so the published predicate is the executed one.

LABEL ISOLATION. Features read only the adapter's quote columns (bid/ask, spot, strike, clocks,
rate, sdiv, ddiv_pv, input_status): srPrc, srVol, vendor IVs and Greeks never reach a predicate.
"""
from __future__ import annotations

import math
import statistics
from pathlib import Path
import sys

import polars as pl

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shape_cohort  # noqa: E402  (sibling tool; imported, never edited)

FEATURE_COLUMNS = ("underlier", "root", "expiry_days", "strike", "is_call", "spot", "bid", "ask",
                   "years", "years_calendar", "sdiv", "ddiv_pv", "input_status")
FRONT_MIN_DTE, FRONT_MAX_DTE = 2, 45
FAR_STRIKE_RATIO = 3.0
FEATURE_DEFINITIONS = {
    "two_sided": "eligible rows (input_status ok) with bid > 0 and ask > 0",
    "spot": "median spot over eligible rows",
    "median_sdiv": "median supplied continuous yield over eligible rows (signed residual carry)",
    "dist_yield_1y": ("ddiv_pv / spot / years_calendar (medians) at the eligible expiry whose "
                      "calendar tenor is closest to one year; None without cash PV"),
    "front_ratio": (f"ATM straddle vol of the nearest expiry with dte in [{FRONT_MIN_DTE}, "
                    f"{FRONT_MAX_DTE}] over shape_cohort's 30-day straddle vol"),
    "kink_ratio": ("max over consecutive expiries of the forward straddle variance "
                   "(w_i - w_j) / (T_i - T_j), w = straddle_vol^2 * years, over its median; "
                   "None with fewer than three straddle expiries or a non-positive median"),
    "far_strike_share": f"share of eligible rows with strike / spot > {FAR_STRIKE_RATIO:g}",
    "straddle_vol": "shape_cohort's 30-day ATM straddle vol proxy (quote mids, never srVol)",
}
# (name, rule). The rules see FEATURE_DEFINITIONS names only.
ARCHETYPE_RULES = (
    ("high_distribution", "dist_yield_1y is not None and dist_yield_1y >= 0.05 and two_sided >= 50"),
    ("htb", "median_sdiv is not None and median_sdiv >= 0.10 and two_sided >= 50"),
    ("neg_carry", "median_sdiv is not None and median_sdiv <= -0.05"),
    ("event_front", "front_ratio is not None and front_ratio >= 1.4 and two_sided >= 100"),
    ("event_kink", "kink_ratio is not None and kink_ratio >= 3.0 and two_sided >= 100"),
    ("strike_ladder", "far_strike_share >= 0.10 and spot is not None and spot >= 20"),
    ("low_straddle_vol", "straddle_vol is not None and straddle_vol < 0.10"
                         " and spot is not None and spot >= 5 and two_sided >= 50"),
)
# Source: issuer product pages (ProShares, Direxion, GraniteShares, Tradr, Defiance, T-Rex,
# Leverage Shares) for 2x/3x and inverse ETPs listed in 2026-10; metadata, not data-derived.
LEVERAGED_ETP_ROSTER = (
    "TQQQ", "SQQQ", "QLD", "QID", "UPRO", "SPXU", "SPXL", "SPXS", "SSO", "SDS", "UDOW", "SDOW",
    "TNA", "TZA", "SOXL", "SOXS", "TECL", "TECS", "FAS", "FAZ", "LABU", "LABD", "ERX", "ERY",
    "GUSH", "DRIP", "NUGT", "DUST", "JNUG", "JDST", "TMF", "TMV", "YINN", "YANG", "FNGU", "FNGD",
    "UVXY", "SVXY", "UVIX", "SVIX", "BOIL", "KOLD", "UCO", "SCO", "AGQ", "ZSL", "TSLL", "TSLQ",
    "TSLZ", "NVDL", "NVDU", "NVDD", "NVDQ", "MSTU", "MSTX", "MSTZ", "CONL", "AAPU", "AAPD",
    "AMZU", "AMDL", "GGLL", "METU", "MSFU", "PLTU", "ETHU", "BITX", "SBIT", "BITU", "TSDD",
    "NVD", "AAPB", "FBL", "PLTY", "MSTY", "XRPN",
)
# Source: depositary-bank DR directories (BNY Mellon, Citi, JPMorgan) for US-listed ADRs in
# 2026-10; metadata, not data-derived.
ADR_ROSTER = (
    "TSM", "BABA", "ASML", "NVO", "SONY", "TM", "HSBC", "BP", "SHEL", "AZN", "GSK", "SAP", "UL",
    "DEO", "BHP", "RIO", "VALE", "PBR", "ITUB", "BBD", "NIO", "LI", "XPEV", "PDD", "JD", "BIDU",
    "NTES", "TCOM", "SE", "INFY", "WIT", "HDB", "IBN", "SNY", "NVS", "BUD", "UBS", "DB", "ING",
    "SAN", "BBVA", "TEF", "PHG", "ERIC", "NOK", "E", "EQNR", "TTE", "ARM", "BTI", "MUFG", "SMFG",
    "HMC", "TAK", "NGG", "BCS", "LYG", "ABEV", "GGB", "SID", "CX", "AMX", "KOF", "FMX", "TV",
    "YPF", "GFI", "AU", "HMY", "SBSW", "BEKE", "TME", "VIPS", "ZTO", "YMM", "GRAB", "CPNG",
)
ROSTERS = {"leveraged_etp": LEVERAGED_ETP_ROSTER, "adr": ADR_ROSTER}


def evaluate(rule: str, names: dict) -> bool:
    """shape_cohort's evaluator: a fixed in-module expression, no builtins, bool or defect."""
    return shape_cohort.evaluate(rule, names)


def _atm_straddles(eligible: pl.DataFrame) -> pl.DataFrame:
    """Per (underlier, expiry): the ATM straddle vol from the strike nearest spot with both
    sides bid > 0 (mids; (C + P) sqrt(2 pi) / 2 / (S sqrt(T)), T = variance years)."""
    two = eligible.filter(pl.col("bid") > 0)
    keys = ["underlier", "root", "expiry_days", "strike"]
    calls = two.filter(pl.col("is_call") == 1).group_by(keys).agg(
        pl.col("mid").min().alias("call"), pl.col("dte").first(), pl.col("spot").median(),
        pl.col("years").median())
    puts = two.filter(pl.col("is_call") == 0).group_by(keys).agg(pl.col("mid").min().alias("put"))
    both = calls.join(puts, on=keys).filter(pl.col("years") > 0).with_columns(
        (pl.col("strike") / pl.col("spot")).log().abs().alias("abs_k"))
    pick = both.sort(["underlier", "expiry_days", "abs_k", "strike", "root"]).group_by(
        ["underlier", "expiry_days"], maintain_order=True).first()
    return pick.with_columns(((pl.col("call") + pl.col("put")) * math.sqrt(2 * math.pi) / 2
                              / (pl.col("spot") * pl.col("years").sqrt())).alias("sv"))


def _kink_ratio(rows: list[tuple[float, float]]) -> float | None:
    """rows: (years, straddle_vol) sorted by years."""
    points = [(t, v * v * t) for t, v in rows if t > 0 and v is not None and math.isfinite(v)]
    fwd = []
    for (t0, w0), (t1, w1) in zip(points, points[1:]):
        if t1 > t0:
            fwd.append((w1 - w0) / (t1 - t0))
    if len(fwd) < 2:
        return None
    med = statistics.median(fwd)
    if not med > 0:
        return None
    return max(fwd) / med


def features(quotes: pl.DataFrame) -> pl.DataFrame:
    """Per-underlier label-free archetype features of one snapshot's quote frame (must carry
    FEATURE_COLUMNS and a calendar `dte` column, as shape_cohort.load_quotes provides)."""
    eligible = quotes.filter(pl.col("input_status") == "ok").with_columns(
        ((pl.col("bid") + pl.col("ask")) * 0.5).alias("mid"))
    base = eligible.group_by("underlier").agg(
        ((pl.col("bid") > 0) & (pl.col("ask") > 0)).sum().alias("two_sided"),
        pl.col("spot").median().alias("spot"),
        pl.col("sdiv").median().alias("median_sdiv"),
        (pl.col("strike") / pl.col("spot") > FAR_STRIKE_RATIO).mean().alias("far_strike_share"))
    per_exp = eligible.group_by("underlier", "expiry_days").agg(
        pl.col("years_calendar").median().alias("tc"), pl.col("ddiv_pv").median().alias("pv"),
        pl.col("spot").median().alias("s")).with_columns((pl.col("tc") - 1.0).abs().alias("gap"))
    one_year = per_exp.filter(pl.col("tc") > 0).sort(["underlier", "gap", "expiry_days"]).group_by(
        "underlier", maintain_order=True).first().with_columns(
        pl.when(pl.col("pv") > 0).then(pl.col("pv") / pl.col("s") / pl.col("tc"))
        .otherwise(None).alias("dist_yield_1y")).select("underlier", "dist_yield_1y")
    straddles = _atm_straddles(eligible)
    thirty = shape_cohort.straddle_vols(eligible)
    front = straddles.filter((pl.col("dte") >= FRONT_MIN_DTE) & (pl.col("dte") <= FRONT_MAX_DTE)) \
        .sort(["underlier", "dte"]).group_by("underlier", maintain_order=True).first() \
        .select("underlier", pl.col("sv").alias("front_sv"))
    front_map = dict(front.iter_rows())
    kinks: dict[str, float | None] = {}
    for (name,), group in straddles.sort(["underlier", "years"]).group_by(["underlier"],
                                                                          maintain_order=True):
        kinks[name] = _kink_ratio(list(zip(group["years"].to_list(), group["sv"].to_list())))
    frame = base.join(one_year, on="underlier", how="left").sort("underlier")
    records = []
    for row in frame.to_dicts():
        name = row["underlier"]
        sv30 = thirty.get(name, (None, None))[0]
        fsv = front_map.get(name)
        row["straddle_vol"] = sv30
        row["front_ratio"] = (fsv / sv30) if (fsv is not None and sv30 and sv30 > 0) else None
        row["kink_ratio"] = kinks.get(name)
        records.append(row)
    return pl.DataFrame(records, infer_schema_length=None)


def archetypes(row: dict) -> list[str]:
    """Archetype names of one feature row (rules in ARCHETYPE_RULES order, then rosters)."""
    names = {k: row.get(k) for k in FEATURE_DEFINITIONS}
    out = [name for name, rule in ARCHETYPE_RULES if evaluate(rule, names)]
    out.extend(name for name, roster in ROSTERS.items() if row.get("underlier") in roster)
    return out
