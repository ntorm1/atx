#!/usr/bin/env python3
"""Validate the panel's implied marks against the raw quoted straddles on disk.

WHY THIS EXISTS. The round-11 cleared book (f16 + f5 + earnings veto) leans on
f5_hv_iv_gap = ln(rv_trail / iv_fair_21d), which is a declared entry-mark
channel: a STALE-LOW iv_fair mark inflates the signal and the measured P&L
TOGETHER, and no crossing-cost assumption covers a mark that was never real.
This script is the in-data half of that validation: for the very names the
book buys (top of the f5 ranking) and a random control group, it re-derives an
ATM vol from the raw OPRA quote hive and asks whether the panel's mark is
systematically BELOW the market's on the bought side.

METHOD. For each sampled (symbol, date):
  * pick the OSI expiry closest to 30 calendar days (the 21-trading-day tenor
    the panel marks), inside [21, 45];
  * pick the strike nearest the panel's own spot with a two-sided call AND put;
  * straddle mid -> Brenner-Subrahmanyam ATM inversion
        sigma_quote = straddle / (0.7979 * S * sqrt(T));
  * gap = sigma_quote - iv_atmf_21d (the panel's ATM mark, the like-for-like
    column; iv_fair_21d is a var-swap strip and sits ABOVE ATM by the smile).
The absolute level of the gap carries the approximation (rates, dividends,
discrete strikes, expiry mismatch); the TEST is relative: the same procedure
runs on both groups, so a clean book shows the same gap distribution in both,
and a mark-fiction problem shows the bought group's gap sitting HIGHER
(market above mark exactly where the signal says "cheap").

Reads only local files. No network.
"""
from __future__ import annotations

import argparse
import datetime as dt
import math
import random
import statistics
import sys
from pathlib import Path

PX_SCALE = 1e-9
BS_ATM = 0.7978845608028654  # 2 * norm.pdf(0) -- straddle ~ BS_ATM * S * sigma * sqrt(T)


def parse_osi(sym: str) -> tuple[dt.date, str, float] | None:
    """'AMD   260313C00040000' -> (2026-03-13, 'C', 40.0). None if malformed."""
    if len(sym) < 21:
        return None
    tail = sym[-15:]
    try:
        expiry = dt.date(2000 + int(tail[0:2]), int(tail[2:4]), int(tail[4:6]))
        cp = tail[6]
        strike = int(tail[7:15]) / 1000.0
    except ValueError:
        return None
    if cp not in ("C", "P"):
        return None
    return expiry, cp, strike


def quote_atm_vol(rows: list[dict], spot: float, on_date: dt.date) -> tuple[float, int] | None:
    """Brenner-Subrahmanyam ATM vol from the tightest usable straddle.

    Returns (sigma, dte_calendar) or None. Requires a two-sided call and put at
    the same strike, expiry in [21, 45] calendar days, strike within 10% of
    spot -- the approximation degrades fast away from the money.
    """
    by_key: dict[tuple[dt.date, float], dict[str, float]] = {}
    for r in rows:
        parsed = parse_osi(r["symbol"])
        if parsed is None:
            continue
        expiry, cp, strike = parsed
        dte = (expiry - on_date).days
        if dte < 21 or dte > 45:
            continue
        if abs(strike - spot) > 0.10 * spot:
            continue
        bid = r["bid_px"]
        ask = r["ask_px"]
        # INT64_MIN marks an unset side; a one-sided quote is not a price.
        if bid <= 0 or ask <= 0 or ask < bid:
            continue
        mid = 0.5 * (bid + ask) * PX_SCALE
        by_key.setdefault((expiry, strike), {})[cp] = mid

    best: tuple[float, float, int] | None = None  # (|strike-spot|, sigma, dte)
    target = 30
    for (expiry, strike), legs in by_key.items():
        if "C" not in legs or "P" not in legs:
            continue
        dte = (expiry - on_date).days
        straddle = legs["C"] + legs["P"]
        t_years = dte / 365.0
        sigma = straddle / (BS_ATM * spot * math.sqrt(t_years))
        # Nearest strike first; among equals, nearest to the 30d target tenor.
        key = (abs(strike - spot), abs(dte - target))
        if best is None or key < (best[0], abs(best[2] - target)):
            best = (abs(strike - spot), sigma, dte)
    if best is None:
        return None
    return best[1], best[2]


def load_panel(path: Path) -> list[dict]:
    out: list[dict] = []
    with open(path, encoding="utf-8") as fh:
        header: list[str] | None = None
        for line in fh:
            if line.startswith("#"):
                continue
            cells = line.rstrip("\n").split("\t")
            if header is None:
                header = cells
                continue
            out.append(dict(zip(header, cells)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--panel", type=Path, required=True)
    ap.add_argument("--hive", type=Path, default=Path(r"C:\atx-data\opra-hive"))
    ap.add_argument("--dates", type=int, default=8, help="sessions sampled, spread over the panel")
    ap.add_argument("--top", type=int, default=12, help="names per group per date")
    ap.add_argument("--seed", type=int, default=11)
    args = ap.parse_args()

    try:
        import pyarrow.parquet as pq
        import pyarrow.compute as pc
    except ImportError:
        print("pyarrow required", file=sys.stderr)
        return 2

    rng = random.Random(args.seed)
    panel = load_panel(args.panel)
    by_date: dict[str, list[dict]] = {}
    for row in panel:
        by_date.setdefault(row["date"], []).append(row)
    dates = sorted(by_date)
    if len(dates) < args.dates:
        print(f"panel has only {len(dates)} sessions", file=sys.stderr)
        return 1
    picked = [dates[i * (len(dates) - 1) // (args.dates - 1)] for i in range(args.dates)]

    gaps_top: list[float] = []
    gaps_ctl: list[float] = []
    misses = 0
    print(f"{'date':10} {'grp':4} {'sym':6} {'dte':>3} {'quote':>7} {'mark':>7} {'gap':>7}")
    for d in picked:
        part = args.hive / f"date={d}"
        files = list(part.glob("*.parquet"))
        if not files:
            print(f"{d}: no hive partition", file=sys.stderr)
            continue
        table = pq.read_table(files[0], columns=["underlying", "symbol", "bid_px", "ask_px"])

        rows = by_date[d]
        scored = [r for r in rows if _finite(r.get("f5_hv_iv_gap")) and _finite(r.get("iv_atmf_21d"))]
        if len(scored) < 4 * args.top:
            continue
        scored.sort(key=lambda r: float(r["f5_hv_iv_gap"]), reverse=True)
        top = scored[: args.top]
        ctl = rng.sample(scored[args.top :], args.top)

        for grp, members, sink in (("top", top, gaps_top), ("ctl", ctl, gaps_ctl)):
            for r in members:
                sym = r["symbol"]
                sub = table.filter(pc.equal(table.column("underlying"), sym)).to_pylist()
                spot = float(r["spot"])
                got = quote_atm_vol(sub, spot, dt.date.fromisoformat(d))
                if got is None:
                    misses += 1
                    continue
                sigma_q, dte = got
                mark = float(r["iv_atmf_21d"])
                gap = sigma_q - mark
                # A 50-vol-point mark-vs-quote gap does not exist in a listed
                # name; it is a broken lookup (coarse strike grid on a penny
                # name, a post-split spot against unadjusted OSI strikes).
                # Excluded as measurement failure, counted, never averaged.
                if abs(gap) > 0.50:
                    misses += 1
                    continue
                sink.append(gap)
                print(f"{d:10} {grp:4} {sym:6} {dte:3d} {sigma_q:7.4f} {mark:7.4f} {gap:+7.4f}")

    print()
    for name, g in (("TOP-f5 (the bought names)", gaps_top), ("CONTROL", gaps_ctl)):
        if not g:
            print(f"{name}: no observations")
            continue
        s = sorted(g)
        k = max(1, len(s) // 10)
        trimmed = s[k:-k] if len(s) > 2 * k else s
        print(f"{name}: n={len(g)} mean={statistics.mean(g):+.4f} "
              f"trim10={statistics.mean(trimmed):+.4f} "
              f"median={statistics.median(g):+.4f} stdev={statistics.pstdev(g):.4f}")
    if gaps_top and gaps_ctl:
        diff = statistics.mean(gaps_top) - statistics.mean(gaps_ctl)
        se = math.sqrt(
            statistics.pvariance(gaps_top) / len(gaps_top)
            + statistics.pvariance(gaps_ctl) / len(gaps_ctl)
        )
        t = diff / se if se > 0 else float("nan")
        print(f"\nMARK-FICTION TEST  mean gap difference (top - control) = {diff:+.4f}  t = {t:+.2f}")
        if math.isfinite(t) and t > 2.0 and diff > 0:
            print("  FIRED: the market quotes ABOVE our mark exactly where f5 says 'cheap' --")
            print("  the bought edge is partly a mark artifact, not a trade.")
        else:
            print("  Null: no differential understatement on the bought side at this sample --")
            print("  the marks track the quoted straddles the same in both groups.")
    print(f"\nmisses (no usable straddle): {misses}")
    return 0


def _finite(v) -> bool:
    try:
        f = float(v)
    except (TypeError, ValueError):
        return False
    return math.isfinite(f)


if __name__ == "__main__":
    sys.exit(main())
