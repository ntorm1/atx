#!/usr/bin/env python3
"""iteration16_equity_scorecard.py -- frozen cp16 alpha scorecard over equity-ic ic.csv.

FROZEN recipe (ruling R16-7 as frozen by the cp16 design; no statistical knob is exposed, and
receipt.json["recipe"] restates every constant). Per (signal, horizon h, variant, restriction, year, cut): ic.csv
rows with spread_emitted truthy AND spread_net non-blank, in date order; sub-series = positions 0, h, 2h, ...
(offset 0); n_obs = its length; sharpe = mean/stdev(ddof=1)*sqrt(252/h). sd == 0 or n_obs < 2 -> sharpe_net blank,
reportable 0, note "zero-dispersion"; reportable iff n_obs >= 8 and the sharpe exists; an unreportable row still
prints its point estimate with a blank CI and never feeds an acceptance bar; n_obs < 20 earns the note "wide
interval -- indicative only". CI = circular block bootstrap, block 5, 2,000 draws, percentiles 2.5/97.5 (cp14
convention), nearest-rank round-half-up on the sorted finite draws. Pooled row (year == "pooled") = the same
statistic on the concatenation of the per-year offset-0 sub-series in year order; sign_matches_pool and
sign_stability k/n over reportable years (pooled row only); robustness-only offset_min/max_sharpe over offsets 0..h-1,
no CI; implied_turnover = 1 - rho_rank from signal_autocorr.csv; hole_flag per R16-9. random.Random(20260920) is
seeded once per (signal, h, variant, restriction, cut) group; groups are walked signals
in cp14 ic.csv token order / horizons ascending / variant then restriction in ic.csv token order / cuts 1000 then
3000, and draws are consumed by the per-year rows in year order then the pooled row. Outputs into --out (must not
exist): scorecard.csv, scorecard.md, capacity.csv (R17-8 cost/capacity view, a caveat, not a bar), receipt.json.
--self-test runs the stdlib oracle, the only oracle.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import random
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path

SEED, DRAWS, BLOCK, MIN_OBS, WIDE_OBS = 20260920, 2000, 5, 8, 20
ANNUAL_DAYS, LO_PERMILLE, HI_PERMILLE = 252.0, 25, 975
CP16_DESIGN_SHA256 = "a83484037cae527e2fbfba5dce04289742d1c81666b2c03c132f3da9268c04d1"
MEMBERSHIP_RULE, ANCHOR_CUT = "year-union-not-as-of", "cp14_anchor"
NOT_FIT_CELLS = {(2017, "3000"): "NOT FIT 5,072 > 4,096"}
# 2018 carries the hole flag for every signal whose lookback reaches 252 sessions back into the hole.
HOLE_YEARS, HOLE_2018_SIGNALS = (2016, 2017), ("momentum_252", "blend_equal", "high52_proximity")
EULER_GAMMA = 0.5772156649015329
HEADLINE_HORIZONS = (1, 5, 10, 21, 63)
TURNOVER_SOURCE = "1 - rho_rank from signal_autocorr.csv (per signal, lag 1)"
WIDE_NOTE = "wide interval -- indicative only"
CAPACITY_K_BPS, CAPACITY_HORIZON = (1, 2, 5), 21
CAPACITY_COLUMNS = ["signal", "cut", "horizon", "variant", "restriction", "n_cells", "adv_top_usd", "adv_bottom_usd",
                    "advrank_top", "advrank_bottom", "one_way_turnover", "sharpe_net_k0", "sharpe_net_k1",
                    "sharpe_net_k2", "sharpe_net_k5"]
CAPACITY_DEFINITION = ("illiquidity surcharge per rebalance s_k = k*1e-4 * decile_one_way_turnover * 0.5 *"
                       " (1/advrank_top + 1/advrank_bottom), advrank = rank of the decile's mean_dollar_adv among the"
                       " block's 10 deciles (ascending, 1 = least liquid) / 10, decile 0 = long leg, decile 9 = short"
                       " leg, computed PER CELL from that cell's own quantile_spread.csv and subtracted from every"
                       " element of that cell's offset-0 stride-21 net sub-series before the score() pooling; a cell"
                       " without the mean_dollar_adv column pools unsurcharged; k = 0 reproduces pooled sharpe_net")
COLUMNS = ["signal", "horizon", "variant", "restriction", "year", "cut", "n_obs", "sharpe_net", "ci_lo", "ci_hi",
           "reportable", "hole_flag", "sign_matches_pool", "sign_stability", "rank_ic_mean", "implied_turnover",
           "breadth_mean", "offset_min_sharpe", "offset_max_sharpe", "sharpe_gross", "gross_ci_lo", "gross_ci_hi",
           "dsr_prob", "sr0_ann", "declared_n", "membership_rule", "note"]
RECIPE = {"ruling": "R16-7", "seed": SEED, "bootstrap_draws": DRAWS, "block_length": BLOCK, "sd_ddof": 1,
          "min_obs_reportable": MIN_OBS, "wide_interval_obs_floor": WIDE_OBS, "offset": 0,
          "annualisation_days": ANNUAL_DAYS, "percentiles": [2.5, 97.5], "membership_rule": MEMBERSHIP_RULE,
          "percentile_method": "nearest-rank, round-half-up, on the sorted finite draws (rank = round(p*B), 1-based)",
          "turnover_source": TURNOVER_SOURCE, "not_fit_cells": {"2017_t3000": NOT_FIT_CELLS[(2017, "3000")]},
          "zero_dispersion": "sd == 0 or n_obs < 2 -> sharpe_net blank, reportable 0, note zero-dispersion",
          "gross": "sharpe_gross = the same statistic on spread_gross; pooled gross CI uses a SEPARATE"
                   " random.Random(20260921) per group so the net draws are byte-identical to cp16",
          "deflated_sharpe": "Bailey & Lopez de Prado DSR on the pooled net series: SR per period = mean/sd(ddof=1);"
                             " SR0 = sqrt(V) * ((1-g) * ppf(1-1/N) + g * ppf(1-1/(N e))) with V = sample variance"
                             " of the annualised pooled net Sharpe across every reportable pooled configuration of"
                             " the same cut, de-annualised by sqrt(252/h); DSR = Phi((SR-SR0) sqrt(T-1) /"
                             " sqrt(1 - skew SR + (kurt-1)/4 SR^2)), population skew/kurtosis of the pooled series,"
                             " T = n_obs; N = --declared-n (cumulative declared configurations); reported beside"
                             " the R16-8 bars, never a bar",
          "seed_scope": "one random.Random(20260920) per (signal, horizon, variant, restriction, cut); groups walked"
                        " signals in cp14 ic.csv token order, horizons ascending, variant then restriction in ic.csv"
                        " token order, cuts 1000 then 3000; draws consumed by per-year rows in year order then pooled",
          "capacity": {"ruling": "R17-8", "role": "caveat, not a bar", "horizon": CAPACITY_HORIZON,
                       "k_bps": [0] + list(CAPACITY_K_BPS), "sharpe": "pooled point estimate, no bootstrap",
                       "definition": CAPACITY_DEFINITION}}

def sha256_file(path: Path):
    try:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1 << 20), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError: return None

def truthy(text) -> bool: return str(text).strip() not in ("", "0", "false", "False")
def stride(values, horizon, offset): return values[offset::horizon]
def norm(text) -> str: return str(text).replace("_", "").replace("-", "").lower()
def mean_or_blank(values): return statistics.fmean(values) if values else ""
def fwd(path) -> str: return str(path).replace("\\", "/")
def fmt(value, digits=6): return "" if value in (None, "") else round(float(value), digits)
def pick(rows, **w): return [r for r in rows if all(str(r.get(k)) == str(v) for k, v in w.items())]
def hole_flag(signal, year): return 1 if year in HOLE_YEARS or (year == 2018 and signal in HOLE_2018_SIGNALS) else 0

def sharpe(values, horizon):
    """Annualised Sharpe of a stride-h sub-series; None when sd == 0 or n_obs < 2.
    mean/sd(ddof=1) in compensated float arithmetic (math.fsum): 7x faster than statistics.stdev's
    exact-rational path and verified equal at the published 6 dp on every cp16 scorecard row."""
    count = len(values)
    if count < 2: return None
    mean = math.fsum(values) / count
    sum_sq = math.fsum((v - mean) * (v - mean) for v in values)
    if sum_sq == 0.0: return None
    return mean / math.sqrt(sum_sq / (count - 1)) * ((ANNUAL_DAYS / horizon) ** 0.5)

def nearest_rank(sorted_values, permille):
    """Nearest-rank percentile, round-half-up: rank = round(p*n), 1-based, clamped."""
    return sorted_values[min(max((permille * len(sorted_values) + 500) // 1000 - 1, 0), len(sorted_values) - 1)]

def norm_cdf(x): return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))

def norm_ppf(p):
    """Acklam's rational approximation (|rel err| < 1.15e-9), refined by one Newton step on erf."""
    if not 0.0 < p < 1.0: raise ValueError("ppf domain")
    a = (-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02,
         -3.066479806614716e+01, 2.506628277459239e+00)
    b = (-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01,
         -1.328068155288572e+01)
    c = (-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00,
         4.374664141464968e+00, 2.938163982698783e+00)
    d = (7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00)
    lo, hi = 0.02425, 1.0 - 0.02425
    if p < lo:
        q = math.sqrt(-2.0 * math.log(p))
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0)
    elif p <= hi:
        q = p - 0.5
        r = q * q
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0)
    else:
        q = math.sqrt(-2.0 * math.log(1.0 - p))
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0)
    e = norm_cdf(x) - p
    return x - e * math.sqrt(2.0 * math.pi) * math.exp(x * x / 2.0)

def expected_max_sharpe(variance, trials):
    """E[max SR] over `trials` independent zero-skill trials with Sharpe variance `variance` (per period)."""
    if trials is None or trials < 1 or variance is None or variance <= 0.0: return None
    if trials == 1: return 0.0
    return math.sqrt(variance) * ((1.0 - EULER_GAMMA) * norm_ppf(1.0 - 1.0 / trials)
                                  + EULER_GAMMA * norm_ppf(1.0 - 1.0 / (trials * math.e)))

def deflated_sharpe(values, sr0_per_period):
    """DSR probability that the per-period Sharpe of `values` exceeds sr0; None when undefined."""
    count = len(values)
    if count < 3 or sr0_per_period is None: return None
    mean = statistics.fmean(values)
    sd = statistics.stdev(values)
    if sd == 0.0: return None
    sr = mean / sd
    pop_var = statistics.pvariance(values, mean)
    if pop_var <= 0.0: return None
    skew = sum((v - mean) ** 3 for v in values) / count / pop_var ** 1.5
    kurt = sum((v - mean) ** 4 for v in values) / count / pop_var ** 2
    inner = 1.0 - skew * sr + (kurt - 1.0) / 4.0 * sr * sr
    if inner <= 0.0: return None
    return norm_cdf((sr - sr0_per_period) * math.sqrt(count - 1.0) / math.sqrt(inner))

def bootstrap_ci(values, horizon, rng):
    """Circular block bootstrap CI: block 5, 2,000 draws, nearest-rank 2.5/97.5."""
    count = len(values)
    if count < 2: return None, None
    blocks, draws = -(-count // BLOCK), []
    for _ in range(DRAWS):
        sample = []
        for _block in range(blocks):
            start = rng.randrange(count)
            sample.extend(values[(start + step) % count] for step in range(BLOCK))
        estimate = sharpe(sample[:count], horizon)
        if estimate is not None: draws.append(estimate)
    if len(draws) < 2: return None, None
    draws.sort()
    return nearest_rank(draws, LO_PERMILLE), nearest_rank(draws, HI_PERMILLE)

def offset_range(series_list, horizon):
    """Robustness only: (min, max) Sharpe over offsets 0..h-1 of the concatenated series."""
    seen = [s for s in (sharpe([v for one in series_list for v in stride(one, horizon, o)], horizon)
                        for o in range(horizon)) if s is not None]
    return (fmt(min(seen)), fmt(max(seen))) if seen else ("", "")

def load_cell(cell_dir: Path) -> dict:
    """series/stats keyed by (signal, horizon, variant, restriction); turnover keyed by signal."""
    cell = {"series": {}, "series_gross": {}, "stats": {}, "turnover": {}, "signal": [], "variant": [],
            "restriction": [], "adv": {}, "adv_turnover": {}, "adv_present": False}
    with (cell_dir / "ic.csv").open("r", encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            for field in ("signal", "variant", "restriction"):
                if row[field] not in cell[field]: cell[field].append(row[field])
            key = (row["signal"], int(row["horizon"]), row["variant"], row["restriction"])
            bucket = cell["stats"].setdefault(key, {"rank_ic": [], "n_used": []})
            if truthy(row.get("emitted")):
                for field in ("rank_ic", "n_used"):
                    if str(row.get(field, "")).strip(): bucket[field].append(float(row[field]))
            if truthy(row.get("spread_emitted")) and str(row.get("spread_net", "")).strip():
                cell["series"].setdefault(key, []).append((int(row["date_index"]), float(row["spread_net"])))
                if str(row.get("spread_gross", "")).strip():
                    cell["series_gross"].setdefault(key, []).append((int(row["date_index"]), float(row["spread_gross"])))
    for field in ("series", "series_gross"):
        for key in cell[field]:
            cell[field][key] = [v for _i, v in sorted(cell[field][key], key=lambda p: p[0])]
    # signal_autocorr.csv is keyed by (signal, lag) ONLY -- it has no horizon/variant/restriction
    # columns -- so one implied-turnover value serves every horizon of a signal.
    autocorr = cell_dir / "signal_autocorr.csv"
    if autocorr.exists():
        with autocorr.open("r", encoding="utf-8", newline="") as stream:
            for row in csv.DictReader(line for line in stream if not line.startswith("#")):
                if str(row.get("rho_rank", "")).strip() and str(row.get("lag", "1")).strip() == "1":
                    cell["turnover"][row["signal"]] = 1.0 - float(row["rho_rank"])
    # quantile_spread.csv (cp18+) carries mean_dollar_adv per decile row and decile_one_way_turnover on the SPREAD
    # row; cp17 files lack the column and leave adv/adv_turnover empty (the capacity view degrades gracefully).
    spread = cell_dir / "quantile_spread.csv"
    if spread.exists():
        with spread.open("r", encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(line for line in stream if not line.startswith("#"))
            if reader.fieldnames and "mean_dollar_adv" in reader.fieldnames:
                cell["adv_present"] = True
                for row in reader:
                    key = (row["signal"], int(row["horizon"]), row["variant"], row["restriction"])
                    quantile = str(row.get("quantile", "")).strip()
                    if quantile == "SPREAD":
                        if str(row.get("decile_one_way_turnover", "")).strip():
                            cell["adv_turnover"][key] = float(row["decile_one_way_turnover"])
                    elif quantile.isdigit() and str(row.get("mean_dollar_adv", "")).strip():
                        cell["adv"].setdefault(key, {})[int(quantile)] = float(row["mean_dollar_adv"])
    return cell

def walk_order(cells):
    """(keys in cp14 token order, cuts 1000 then 3000) -- the deterministic RNG walk."""
    order = {field: [] for field in ("signal", "variant", "restriction")}
    for _year, _cut, cell in cells:
        for field in order:
            for token in cell[field]:
                if token not in order[field]: order[field].append(token)
    place = lambda field, token: order[field].index(token) if token in order[field] else 99
    keys = sorted({key for _y, _c, cell in cells for key in cell["series"]},
                  key=lambda k: (place("signal", k[0]), k[1], place("variant", k[2]), place("restriction", k[3])))
    cuts = sorted({str(c) for _y, c, _x in cells}, key=lambda c: (0, int(c)) if c.isdigit() else (1, 0))
    return keys, cuts

def score(cells):
    """cells: list of (year, cut, cell dict) -> list of scorecard row dicts."""
    keys, cuts, rows = walk_order(cells) + ([],)
    for cut in cuts:
        by_year = {year: cell for year, this_cut, cell in cells if str(this_cut) == cut}
        for key in keys:
            signal, horizon, variant, restriction = key
            rng = random.Random(SEED)
            years = sorted(y for y in by_year if by_year[y]["series"].get(key))
            if not years: continue
            def emit(**over):
                rows.append(dict.fromkeys(COLUMNS, "") | dict(
                    signal=signal, horizon=horizon, variant=variant, restriction=restriction,
                    cut=cut, membership_rule=MEMBERSHIP_RULE) | over)
            pooled = [v for y in years for v in stride(by_year[y]["series"][key], horizon, 0)]
            pooled_sharpe, per_year = sharpe(pooled, horizon), {}
            gross_years = [y for y in years if by_year[y]["series_gross"].get(key)]
            pooled_gross = [v for y in gross_years for v in stride(by_year[y]["series_gross"][key], horizon, 0)]
            gross_sharpe = sharpe(pooled_gross, horizon) if len(gross_years) == len(years) else None
            gross_lo, gross_hi = (bootstrap_ci(pooled_gross, horizon, random.Random(SEED + 1))
                                  if gross_sharpe is not None and len(pooled_gross) >= MIN_OBS else (None, None))
            for year in years:
                sub = stride(by_year[year]["series"][key], horizon, 0)
                estimate = sharpe(sub, horizon)
                ok = 1 if (len(sub) >= MIN_OBS and estimate is not None) else 0
                per_year[year] = (sub, estimate, ok) + (bootstrap_ci(sub, horizon, rng) if ok else (None, None))
            pooled_ok = int(len(pooled) >= MIN_OBS and pooled_sharpe is not None)
            pooled_lo, pooled_hi = bootstrap_ci(pooled, horizon, rng) if pooled_ok else (None, None)
            good = [y for y in years if per_year[y][2]]
            same = lambda e: pooled_sharpe is not None and (e >= 0) == (pooled_sharpe >= 0)
            for year in years:
                sub, estimate, ok, ci_lo, ci_hi = per_year[year]
                bucket = by_year[year]["stats"].get(key, {"rank_ic": [], "n_used": []})
                omin, omax = offset_range([by_year[year]["series"][key]], horizon)
                notes = (["zero-dispersion"] if estimate is None else []) + ([WIDE_NOTE] if len(sub) < WIDE_OBS else [])
                gross_sub = stride(by_year[year]["series_gross"].get(key, []), horizon, 0)
                emit(year=year, n_obs=len(sub), sharpe_net=fmt(estimate), ci_lo=fmt(ci_lo), ci_hi=fmt(ci_hi),
                     sharpe_gross=fmt(sharpe(gross_sub, horizon)) if gross_sub else "",
                     reportable=ok, hole_flag=hole_flag(signal, year), offset_min_sharpe=omin, offset_max_sharpe=omax,
                     sign_matches_pool="" if not ok else int(same(estimate)), note="; ".join(notes),
                     rank_ic_mean=fmt(mean_or_blank(bucket["rank_ic"])),
                     implied_turnover=fmt(by_year[year]["turnover"].get(signal)),
                     breadth_mean=fmt(mean_or_blank(bucket["n_used"]), 3))
            grab = lambda f: [x for y in years for x in by_year[y]["stats"].get(key, {}).get(f, [])]
            omin, omax = offset_range([by_year[y]["series"][key] for y in years], horizon)
            emit(year="pooled", n_obs=len(pooled), sharpe_net=fmt(pooled_sharpe), ci_lo=fmt(pooled_lo),
                 ci_hi=fmt(pooled_hi), hole_flag=1, offset_min_sharpe=omin, offset_max_sharpe=omax,
                 sharpe_gross=fmt(gross_sharpe), gross_ci_lo=fmt(gross_lo), gross_ci_hi=fmt(gross_hi),
                 _pooled=pooled,
                 reportable=pooled_ok, sign_stability="%d/%d" % (sum(1 for y in good if same(per_year[y][1])), len(good)),
                 rank_ic_mean=fmt(mean_or_blank(grab("rank_ic"))), breadth_mean=fmt(mean_or_blank(grab("n_used")), 3),
                 implied_turnover=fmt(mean_or_blank([by_year[y]["turnover"][signal] for y in years
                                                     if signal in by_year[y]["turnover"]])),
                 note="" if pooled_sharpe is not None else "zero-dispersion")
            if (2017, cut) in NOT_FIT_CELLS:
                emit(year=2017, n_obs=0, reportable=0, hole_flag=1, note=NOT_FIT_CELLS[(2017, cut)])
    return rows

def deflate(rows, declared_n):
    """Fill dsr_prob/sr0_ann/declared_n on pooled rows; V is the cross-configuration Sharpe variance per cut."""
    if not declared_n: return
    for cut in sorted({r["cut"] for r in rows}):
        pooled = [r for r in rows if r["cut"] == cut and r["year"] == "pooled" and "_pooled" in r]
        trials = [float(r["sharpe_net"]) for r in pooled if r["reportable"] and r["sharpe_net"] != ""]
        variance = statistics.variance(trials) if len(trials) >= 2 else None
        sr0_ann = expected_max_sharpe(variance, declared_n)
        for row in pooled:
            row["declared_n"] = declared_n
            if sr0_ann is None or not row["reportable"]: continue
            horizon = int(row["horizon"])
            sr0 = sr0_ann / (ANNUAL_DAYS / horizon) ** 0.5
            row["sr0_ann"], row["dsr_prob"] = fmt(sr0_ann), fmt(deflated_sharpe(row["_pooled"], sr0))

def materiality(head, signal, cut):
    """R16-9 materiality at h=21: 2016/2017 outside the union of the 2015 and 2019 CIs."""
    per_year = {str(r["year"]): r for r in pick(head, signal=signal, cut=cut, horizon=21)}
    left, right = per_year.get("2015"), per_year.get("2019")
    if (not left or not right or not left["reportable"] or not right["reportable"]
            or "" in (left["ci_lo"], left["ci_hi"], right["ci_lo"], right["ci_hi"])):
        return "inevaluable"
    low, high = min(float(left["ci_lo"]), float(right["ci_lo"])), max(float(left["ci_hi"]), float(right["ci_hi"]))
    for row in (per_year.get(y) for y in ("2016", "2017")):
        if row and row["sharpe_net"] != "" and not low <= float(row["sharpe_net"]) <= high: return "FIRES"
    return "does not fire"

def cell_capacity(cell, key):
    """(advrank_top, advrank_bottom, adv_top, adv_bottom, turnover) from the cell's own quantile_spread.csv, or None
    when decile 0, decile 9 or the SPREAD turnover is missing; rank ascending among the block's deciles, /10."""
    adv, turnover = cell["adv"].get(key), cell["adv_turnover"].get(key)
    if not adv or 0 not in adv or 9 not in adv or turnover is None: return None
    rank = lambda q: (1 + sum(1 for v in adv.values() if v < adv[q])) / 10.0
    return rank(0), rank(9), adv[0], adv[9], turnover

def surcharge(cap, k):
    """Illiquidity surcharge per rebalance for k bps (see CAPACITY_DEFINITION); 0 for a cell without data."""
    return 0.0 if cap is None else k * 1e-4 * cap[4] * 0.5 * (1.0 / cap[0] + 1.0 / cap[1])

def capacity_rows(cells, variant, restriction, horizon=CAPACITY_HORIZON):
    """R17-8 cost/capacity view (caveat, not a bar): per (signal, cut) at the headline variant/restriction, the pooled
    net Sharpe point estimate after subtracting each cell's own surcharge s_k from every element of that cell's
    offset-0 stride-h sub-series; the pooling is score()'s, so k = 0 reproduces the scorecard's pooled sharpe_net."""
    keys, cuts, rows = walk_order(cells) + ([],)
    for cut in cuts:
        by_year = {year: cell for year, this_cut, cell in cells if str(this_cut) == cut}
        for key in keys:
            signal = key[0]
            if key[1:] != (horizon, variant, restriction): continue
            years = sorted(y for y in by_year if by_year[y]["series"].get(key))
            if not years: continue
            subs = {y: stride(by_year[y]["series"][key], horizon, 0) for y in years}
            caps = {y: cell_capacity(by_year[y], key) for y in years}
            with_data = [y for y in years if caps[y] is not None]
            row = dict.fromkeys(CAPACITY_COLUMNS, "") | dict(
                signal=signal, cut=cut, horizon=horizon, variant=variant, restriction=restriction,
                n_cells=len(with_data), sharpe_net_k0=fmt(sharpe([v for y in years for v in subs[y]], horizon)))
            if with_data:
                col = lambda i: fmt(statistics.fmean([caps[y][i] for y in with_data]))
                row.update(advrank_top=col(0), advrank_bottom=col(1), adv_top_usd=col(2), adv_bottom_usd=col(3),
                           one_way_turnover=col(4))
                for k in CAPACITY_K_BPS:
                    row["sharpe_net_k%d" % k] = fmt(sharpe([v - surcharge(caps[y], k) for y in years for v in subs[y]],
                                                           horizon))
            rows.append(row)
    return rows

CAVEATS = (
    "Membership is the YEAR UNION, not as-of: a name that joined mid-year is admitted for the whole year.",
    "19 corrupted pre-holiday sessions (2016-01-15..2018-02-16) contaminate 2016 and 2017 (all signals) and"
    " momentum_252/blend_equal 2018; those rows carry hole_flag=1 and `*`, as does every pooled row (R16-9).",
    "2013 is a PARTIAL year: evaluation starts 2013-04-04, not 2013-01-01.",
    "2014-2019 IncludeAuditedTerminalV1 is expected to collapse onto DropMissingForward; _ex34 is 2013-only (R16-6).",
    "N_14 = 30 configurations (cp14 canonical ledger); cp17 adds N_17 = 7 families x 5 x 2 x 2 = 140 in the cp17 sidecar"
    " ledger; year x cut cells are AR-7 restrictions, not trials (R16-2). Deflated Sharpe uses --declared-n.",
    "No capacity, borrow-availability or impact model: decile spreads at +/-1.0/n gross-2.0 weights only.",
    "`~` marks a per-year cell with n_obs < 20: " + WIDE_NOTE + ".",
    "implied_turnover is " + TURNOVER_SOURCE + "; that file has no horizon/variant/restriction columns, so one value"
    " serves every horizon of a signal.")

def render_md(rows, anchor_rows, variant, restriction, out: Path, sources, title, n_note, declared_n,
              capacity=None, adv_present=0, adv_absent=0):
    add = ["# %s" % title, "",
           "Headline variant `%s`, restriction `%s`. Net of cost. `*` = hole-flagged year, `~` = n_obs < 20 (%s)."
           " Sharpe = mean/sd(ddof=1) x sqrt(252/h) on the offset-0 stride-h sub-series; CI = circular block bootstrap"
           " (block 5, 2,000 draws, seed 20260920, nearest-rank 2.5/97.5)." % (variant, restriction, WIDE_NOTE), ""]
    head = [r for r in rows if r["variant"] == variant and r["restriction"] == restriction]
    signals, cuts = sorted({r["signal"] for r in head}), sorted({r["cut"] for r in head})
    for cut in cuts:
        for signal in signals:
            add += ["## %s -- cut %s" % (signal, cut),
                    "| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi"
                    " | n_obs | sign stab | DSR |", "|---|---|---|---|---|---|---|---|---|---|"]
            for horizon in HEADLINE_HORIZONS:
                for row in pick(head, signal=signal, cut=cut, horizon=horizon, year="pooled"):
                    add.append("| %d | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                        horizon, row["sharpe_net"], row["ci_lo"], row["ci_hi"], row["sharpe_gross"],
                        row["gross_ci_lo"], row["gross_ci_hi"], row["n_obs"], row["sign_stability"], row["dsr_prob"]))
            per_year = sorted([r for r in pick(head, signal=signal, cut=cut, horizon=21) if r["year"] != "pooled"],
                              key=lambda r: int(r["year"]))
            cells = ["%s NOT FIT" % r["year"] if r["n_obs"] == 0 and "NOT FIT" in str(r["note"]) else
                     "%s%s%s %s" % (r["year"], "*" if r["hole_flag"] else "",
                                    "~" if WIDE_NOTE in str(r["note"]) else "", r["sharpe_net"]) for r in per_year]
            add += ["", "- per-year Sharpe @ h=21: " + "; ".join(cells)]
            for row in pick(head, signal=signal, cut=cut, horizon=21, year="pooled"):
                add.append("- rank-IC mean @ h=21: %s | implied turnover (1-rho_rank): %s | breadth (mean n_used): %s"
                           " | offsets [%s, %s]" % (row["rank_ic_mean"], row["implied_turnover"], row["breadth_mean"],
                                                    row["offset_min_sharpe"], row["offset_max_sharpe"]))
            add += ["- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): %s" % materiality(head, signal, cut), ""]
    add += ["## Acceptance bars (R16-8) -- passing = candidate, not tradeable",
            "| signal | bar1 pooled ci_lo (2.5%) > 0 @ h=21 both cuts | bar2 sign stability n/n | bar3 turnover"
            " printed | verdict | DSR @ h=21 per cut (N=" + str(declared_n or "n/a") + ", beside the bars, not a bar) |",
            "|---|---|---|---|---|---|"]
    for signal in signals:
        pooled = [p[0] for p in (pick(head, signal=signal, cut=c, horizon=21, year="pooled") for c in cuts) if p]
        full = bool(pooled) and len(pooled) == len(cuts)
        bars = [full and all(r["reportable"] and r["ci_lo"] != "" and float(r["ci_lo"]) > 0 for r in pooled),
                full and all(r["sign_stability"] and r["sign_stability"].split("/")[0]
                             == r["sign_stability"].split("/")[1] for r in pooled),
                full and all(r["implied_turnover"] != "" for r in pooled)]
        add.append("| %s | %s | %s | %s |" % (signal, " | ".join("PASS" if b else "FAIL" for b in bars),
                                              "PASS" if all(bars) else "FAIL",
                                              "; ".join("t%s %s" % (r["cut"], r["dsr_prob"] if r["dsr_prob"] != "" else "n/a")
                                                        for r in pooled)))
    add += ["", "Passing = candidate, not tradeable.", "", "## cp14 2013 anchor (same recipe, cp14 context -- NOT"
            " a cp16 cell, R16-5)", "| signal | h | year | Sharpe | ci_lo | ci_hi | n_obs |", "|---|---|---|---|---|---|---|"]
    for row in anchor_rows:
        if (row["variant"] == variant and row["restriction"] == restriction
                and row["horizon"] in HEADLINE_HORIZONS and row["year"] == "pooled"):
            add.append("| %s | %s | 2013 | %s | %s | %s | %s |" % (row["signal"], row["horizon"], row["sharpe_net"],
                                                                   row["ci_lo"], row["ci_hi"], row["n_obs"]))
    add += ["", "## Cost/capacity view (caveat, not a bar; R17-8)"]
    if not adv_present or not capacity:
        add.append("column mean_dollar_adv absent in %d cells (all of them): no cost/capacity view for this run;"
                   " quantile_spread.csv predates the column." % adv_absent)
    else:
        money = lambda v: "" if v == "" else "%.1f" % (float(v) / 1e6)
        two = lambda v: "" if v == "" else "%.2f" % float(v)
        add += ["| signal | cut | h | variant | restriction | n_cells | ADV top ($M) | ADV bottom ($M) | advrank top"
                " | advrank bottom | one-way turnover | Sharpe k=0 | k=1 bps | k=2 bps | k=5 bps |",
                "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for row in sorted(capacity, key=lambda r: (r["signal"], str(r["cut"]))):
            add.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                row["signal"], row["cut"], row["horizon"], row["variant"], row["restriction"], row["n_cells"],
                money(row["adv_top_usd"]), money(row["adv_bottom_usd"]), two(row["advrank_top"]),
                two(row["advrank_bottom"]), two(row["one_way_turnover"]), two(row["sharpe_net_k0"]),
                two(row["sharpe_net_k1"]), two(row["sharpe_net_k2"]), two(row["sharpe_net_k5"])))
        add += ["", "- Surcharge: " + CAPACITY_DEFINITION + "."]
        if adv_absent: add.append("- column mean_dollar_adv absent in %d cells (those cells pool unsurcharged and"
                                  " are excluded from n_cells and the ADV/advrank/turnover means)." % adv_absent)
    add += ["", "## Caveats (all load-bearing)"] + ["- " + c for c in CAVEATS] + (["- " + n_note] if n_note else []) + [
        "- Sources: " + "; ".join(sources)]
    out.write_text("\n".join(add) + "\n", encoding="utf-8")

def self_test() -> int:
    """The only oracle: 4 hand-derivable series plus the stride and percentile primitives."""
    assert sharpe([0.01] * 10, 1) is None, "constant series -> sd 0 -> zero-dispersion"
    assert abs(sharpe([1.0, -1.0] * 5, 1)) < 1e-12, "alternating +/-1 -> mean 0 -> Sharpe 0"
    known = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]  # mean 4.5, sd(ddof=1) = sqrt(6)
    assert abs(sharpe(known, 1) - 4.5 / 6.0 ** 0.5 * 252.0 ** 0.5) < 1e-12, "known mean/sd Sharpe formula"
    assert abs(sharpe(known, 1) - 29.163332) < 1e-5, "known Sharpe should be 4.5*sqrt(42) = 29.163332"
    assert len(stride(list(range(1, 8)), 1, 0)) == 7 < MIN_OBS, "n_obs 7 is below the reportable floor"
    assert stride(list(range(16)), 2, 0) == [0, 2, 4, 6, 8, 10, 12, 14], "stride-h sub-series extraction"
    assert nearest_rank(list(range(1, 2001)), LO_PERMILLE) == 50, "2.5th pct of 1..2000 is the 50th draw"
    assert nearest_rank(list(range(1, 2001)), HI_PERMILLE) == 1950, "97.5th pct of 1..2000 is the 1950th draw"
    assert nearest_rank([1, 2, 3, 4], LO_PERMILLE) == 1, "rank 0 clamps to the first draw"
    low, high = bootstrap_ci(known, 1, random.Random(SEED))
    assert low is not None and high is not None and low <= high, "bootstrap CI must be ordered"
    assert abs(norm_ppf(0.975) - 1.959963984540054) < 1e-9, "ppf(0.975) = 1.959964"
    assert abs(norm_cdf(1.959963984540054) - 0.975) < 1e-12, "cdf(1.959964) = 0.975"
    assert expected_max_sharpe(1.0, 1) == 0.0, "one trial deflates nothing"
    assert abs(expected_max_sharpe(1.0, 100) - ((1 - EULER_GAMMA) * norm_ppf(0.99) + EULER_GAMMA * norm_ppf(1 - 1 / (100 * math.e)))) < 1e-12
    assert deflated_sharpe([0.01] * 10, 0.0) is None, "zero dispersion -> no DSR"
    strong = [0.02, 0.01, 0.03, 0.02, 0.01, 0.025, 0.015, 0.02, 0.03, 0.01, 0.02, 0.02]
    assert deflated_sharpe(strong, 0.0) > 0.99, "an all-positive series beats SR0 = 0 with high probability"
    assert deflated_sharpe(strong, 10.0) < 0.01, "an absurd SR0 is never beaten"
    ckey = ("s", CAPACITY_HORIZON, "v", "r")
    def synth(values, adv_top, adv_bottom, turnover, present=True):
        adv = {q: 1e6 * (10 - q) for q in range(10)}  # deciles 1..8 span 9e6..2e6; 0 and 9 are overridden
        adv[0], adv[9] = adv_top, adv_bottom
        return {"series": {ckey: values}, "series_gross": {}, "stats": {}, "turnover": {}, "signal": ["s"],
                "variant": ["v"], "restriction": ["r"], "adv": {ckey: adv} if present else {},
                "adv_turnover": {ckey: turnover} if present else {}, "adv_present": present}
    rng = random.Random(1)
    a = [0.010 + 0.005 * rng.random() for _ in range(21 * 6)]
    b = [0.020 + 0.005 * rng.random() for _ in range(21 * 8)]
    pair = [(2015, "1000", synth(a, 2e7, 5e5, 1.5)), (2016, "1000", synth(b, 3e7, 4e5, 1.0))]
    cap = capacity_rows(pair, "v", "r")
    assert len(cap) == 1 and cap[0]["n_cells"] == 2 and cap[0]["cut"] == "1000", "one (signal, cut) row, both cells"
    pooled = stride(a, 21, 0) + stride(b, 21, 0)
    assert cap[0]["sharpe_net_k0"] == fmt(sharpe(pooled, 21)) == pick(score(pair), year="pooled")[0]["sharpe_net"], \
        "k = 0 reproduces the scorecard's pooled sharpe_net"
    assert (cap[0]["advrank_top"], cap[0]["advrank_bottom"], cap[0]["adv_top_usd"], cap[0]["one_way_turnover"]) == \
        (1.0, 0.1, 2.5e7, 1.25), "ranks 10/10 and 1/10, means over the two cells"
    # s_1 = 1e-4 * 1.5 * 0.5 * (1/1.0 + 1/0.1) = 8.25e-4 (cell a) and 1e-4 * 1.0 * 0.5 * 11 = 5.5e-4 (cell b)
    assert cap[0]["sharpe_net_k1"] == fmt(sharpe([v - 8.25e-4 for v in stride(a, 21, 0)]
                                                 + [v - 5.5e-4 for v in stride(b, 21, 0)], 21)), "per-cell surcharge"
    assert float(cap[0]["sharpe_net_k5"]) < float(cap[0]["sharpe_net_k2"]) < float(cap[0]["sharpe_net_k1"]) \
        < float(cap[0]["sharpe_net_k0"]), "the surcharge lowers the pooled Sharpe monotonically in k"
    degraded = capacity_rows([pair[0], (2016, "1000", synth(b, 0, 0, 0, present=False))], "v", "r")[0]
    assert degraded["n_cells"] == 1 and degraded["sharpe_net_k0"] == cap[0]["sharpe_net_k0"], "absent column degrades"
    assert capacity_rows(pair, "v", "other") == [], "no headline match -> no capacity rows"
    print("self-test OK: 4 hand-derivable series + stride + nearest-rank 2.5/97.5 + CI ordering + capacity 2-cell")
    return 0

def parse_cells(specs, globs):
    cells = []
    for spec in specs or []:
        directory, year, cut = spec.rsplit(":", 2)
        cells.append((int(year), str(cut), Path(directory)))
    for pattern in globs or []:
        for directory in sorted(Path(pattern).parent.glob(Path(pattern).name)):
            parts = directory.name.split("_")
            if len(parts) >= 5 and parts[3].isdigit() and parts[4].startswith("t"): cells.append(
                    (int(parts[3]), parts[4][1:], directory))
    return cells

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="cp16 frozen alpha scorecard")
    parser.add_argument("--cells", nargs="*", default=[], metavar="DIR:YEAR:CUT")
    parser.add_argument("--glob", nargs="*", default=[], metavar="PATTERN")
    parser.add_argument("--anchor", type=Path, default=Path("C:/atx/data/equity_ic_training_2013_20260920"))
    parser.add_argument("--out", type=Path)
    parser.add_argument("--headline-variant", default="include_audited_terminal_v1")
    parser.add_argument("--headline-restriction", default="full")
    parser.add_argument("--cells-receipt", type=Path, default=None)
    parser.add_argument("--cp16-design", type=Path, default=Path(
        "C:/atx/.worktrees/equity-platform/atx-engine/reviews/2026-09-20-iteration16-alpha-scorecard-design.md"))
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--declared-n", type=int, default=None,
                        help="cumulative declared configuration count N for the deflated Sharpe")
    parser.add_argument("--n-note", default="", help="one caveat line stating how N was accounted")
    parser.add_argument("--title", default="cp16 first alpha scorecard (frozen recipe R16-7) -- ONE PAGE")
    opt = parser.parse_args(argv)
    if opt.self_test: return self_test()
    if opt.out is None: parser.error("--out is required unless --self-test")
    if opt.out.exists():
        print("REFUSED: --out already exists (immutable): %s" % opt.out)
        return 2
    design_sha = sha256_file(opt.cp16_design)
    if design_sha != CP16_DESIGN_SHA256:
        print("REFUSED: cp16 design sha256 %s != frozen %s (%s)" % (design_sha, CP16_DESIGN_SHA256, opt.cp16_design))
        return 4
    specs = parse_cells(opt.cells, opt.glob)
    if not specs: parser.error("no cells given (--cells or --glob)")
    inputs = {fwd(d / n): sha256_file(d / n) for d in [x for _y, _c, x in specs] + [opt.anchor]
              for n in ("ic.csv", "signal_autocorr.csv", "quantile_spread.csv")}
    loaded = [(year, cut, load_cell(directory)) for year, cut, directory in specs]
    rows = score(loaded)
    deflate(rows, opt.declared_n)
    anchor_rows = score([(2013, ANCHOR_CUT, load_cell(opt.anchor))])
    variants, restrictions = {r["variant"] for r in rows + anchor_rows}, {r["restriction"] for r in rows + anchor_rows}
    variant = next((v for v in sorted(variants) if norm(v) == norm(opt.headline_variant)), None)
    restriction = next((r for r in sorted(restrictions) if norm(r) == norm(opt.headline_restriction)), None)
    if variant is None or restriction is None:
        print("REFUSED: headline not found. variants=%s restrictions=%s" % (sorted(variants), sorted(restrictions)))
        return 3
    capacity = capacity_rows(loaded, variant, restriction)
    adv_present = sum(1 for _y, _c, cell in loaded if cell["adv_present"])
    opt.out.mkdir(parents=True, exist_ok=False)
    all_rows = rows + anchor_rows
    with (opt.out / "scorecard.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=COLUMNS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows({c: row.get(c, "") for c in COLUMNS} for row in all_rows)
    with (opt.out / "capacity.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=CAPACITY_COLUMNS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(capacity)
    render_md(rows, anchor_rows, variant, restriction, opt.out / "scorecard.md", sorted(inputs.keys()),
              opt.title, opt.n_note, opt.declared_n, capacity, adv_present, len(loaded) - adv_present)
    receipt = {"script": fwd(Path(__file__).resolve()), "script_sha256": sha256_file(Path(__file__).resolve()),
               "generated_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
               "inputs_sha256": inputs, "anchor": fwd(opt.anchor), "anchor_cut_label": ANCHOR_CUT,
               "cells": [{"year": y, "cut": c, "dir": fwd(d)} for y, c, d in specs],
               "headline": {"variant": variant, "restriction": restriction},
               "cells_receipt": fwd(opt.cells_receipt) if opt.cells_receipt else None,
               "cells_receipt_sha256": sha256_file(opt.cells_receipt) if opt.cells_receipt else None,
               "cp16_design": fwd(opt.cp16_design), "cp16_design_sha256": design_sha,
               "declared_n": opt.declared_n, "n_note": opt.n_note, "title": opt.title,
               "recipe": RECIPE, "rows_written": len(all_rows), "capacity_rows_written": len(capacity),
               "capacity_cells_with_adv": adv_present, "capacity_cells_without_adv": len(loaded) - adv_present,
               "outputs_sha256": {name: sha256_file(opt.out / name) for name in ("scorecard.csv", "capacity.csv")}}
    with (opt.out / "receipt.json").open("x", encoding="utf-8") as stream:
        json.dump(receipt, stream, indent=2)
        stream.write("\n")
    print("wrote %d rows -> %s" % (len(all_rows), fwd(opt.out)))
    print("capacity view: %d rows, mean_dollar_adv present in %d/%d cells" % (len(capacity), adv_present, len(loaded)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
