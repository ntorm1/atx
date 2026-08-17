"""Measure per (date, underlying) OPTION QUOTED SPREAD and DEPTH from the opra-hive.

This is the MEASURED liquidity ground truth the archive-side `n_used` proxy is
validated against (lane/vrp-liquidity). It reads the hive's one daily snapshot
(single `ts` per date partition) and, for every underlying, isolates the
near-21d expiry, locates the ATM strike by put-call parity (no spot column
exists in the hive), and reports the quoted relative half-spread and the
quoted depth there.

Emits a TSV: date, underlying, dte, n_strikes_21d, atm_strike, atm_rel_hspread,
atm_depth, med_rel_hspread_21d, med_depth_21d, n_contracts_total.

Prices are databento fixed-point scaled 1e9; INT64_MIN is the UNDEF sentinel.
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
import os
import sys

import pyarrow.parquet as pq

PX_SCALE = 1e9
UNDEF = -(2**63)


def parse_osi(sym: str) -> tuple[str, dt.date, str, float] | None:
    """OSI: 6-char root, YYMMDD, C/P, 8-digit strike in 1/1000 dollars."""
    if len(sym) != 21:
        return None
    root = sym[:6].strip()
    try:
        exp = dt.datetime.strptime(sym[6:12], "%y%m%d").date()
        right = sym[12]
        strike = int(sym[13:]) / 1000.0
    except ValueError:
        return None
    if right not in ("C", "P"):
        return None
    return root, exp, right, strike


def rel_hspread(bid: int, ask: int) -> float | None:
    """One-way relative half-spread against the mid, or None if unquotable."""
    if bid == UNDEF or ask == UNDEF:
        return None
    b = bid / PX_SCALE
    a = ask / PX_SCALE
    if not (a > 0.0) or b < 0.0 or a < b:
        return None
    mid = 0.5 * (a + b)
    if not (mid > 0.0):
        return None
    return 0.5 * (a - b) / mid


def process_date(path: str, date_str: str, dte_lo: int, dte_hi: int) -> list[dict]:
    t = pq.read_table(
        path, columns=["underlying", "symbol", "bid_px", "ask_px", "bid_sz", "ask_sz"]
    )
    cols = t.to_pydict()
    session = dt.datetime.strptime(date_str, "%Y-%m-%d").date()

    # (underlying, expiry) -> {strike: {right: (bid, ask, bsz, asz)}}
    by_u: dict[str, dict] = {}
    totals: dict[str, int] = {}
    for u, sym, bp, ap, bs, asz in zip(
        cols["underlying"], cols["symbol"], cols["bid_px"],
        cols["ask_px"], cols["bid_sz"], cols["ask_sz"], strict=True
    ):
        totals[u] = totals.get(u, 0) + 1
        p = parse_osi(sym)
        if p is None:
            continue
        _root, exp, right, strike = p
        dte = (exp - session).days
        if dte < dte_lo or dte > dte_hi:
            continue
        by_u.setdefault(u, {}).setdefault(exp, {}).setdefault(strike, {})[right] = (
            bp, ap, bs, asz
        )

    out = []
    for u, expiries in by_u.items():
        # The single expiry nearest 21 calendar days: what a 21d ATM straddle trades.
        exp = min(expiries, key=lambda e: abs((e - session).days - 21))
        dte = (exp - session).days
        strikes = expiries[exp]
        # ATM by put-call parity: |C_mid - P_mid| is minimised at K ~= forward.
        best_k = None
        best_gap = None
        for k, sides in strikes.items():
            if "C" not in sides or "P" not in sides:
                continue
            cb, ca, _, _ = sides["C"]
            pb, pa, _, _ = sides["P"]
            if UNDEF in (cb, ca, pb, pa):
                continue
            cm = 0.5 * (cb + ca) / PX_SCALE
            pm = 0.5 * (pb + pa) / PX_SCALE
            gap = abs(cm - pm)
            if best_gap is None or gap < best_gap:
                best_gap, best_k = gap, k
        hs_all, dep_all = [], []
        for _k, sides in strikes.items():
            for _r, (bp, ap, bs, asz) in sides.items():
                h = rel_hspread(bp, ap)
                if h is not None:
                    hs_all.append(h)
                    dep_all.append(min(bs, asz))
        atm_hs, atm_dep, atm_vp, atm_iv = (float("nan"),) * 4
        if best_k is not None:
            hs, dep, absh, mids = [], [], [], []
            for r in ("C", "P"):
                if r in strikes[best_k]:
                    bp, ap, bs, asz = strikes[best_k][r]
                    h = rel_hspread(bp, ap)
                    if h is not None:
                        hs.append(h)
                        dep.append(min(bs, asz))
                        absh.append(0.5 * (ap - bp) / PX_SCALE)
                        mids.append(0.5 * (ap + bp) / PX_SCALE)
            if hs:
                atm_hs = sum(hs) / len(hs)
                atm_dep = sum(dep) / len(dep)
                # THE CLASSIFICATION AXIS. A flat vol-point cost assumption is
                # stated in vol points, so the measurement must be too. Under the
                # Brenner-Subrahmanyam ATM approximation an option's vega per ONE
                # VOL POINT is 0.4*K*sqrt(T)/100, so the one-way half-spread in
                # vol points is the absolute half-spread divided by that. This
                # needs no IV input at all — which matters, because the RELATIVE
                # half-spread is mechanically deflated for high-vol names (same
                # absolute width over a fatter premium) and would mis-sort them
                # as liquid on exactly the axis the cost is charged in.
                tau = max(dte, 1) / 365.0
                vega_pt = 0.4 * best_k * math.sqrt(tau) / 100.0
                if vega_pt > 0.0:
                    atm_vp = (sum(absh) / len(absh)) / vega_pt
                    # Implied ATM vol from the same approximation, reported so a
                    # reader can see the vol level a name's width sits on.
                    atm_iv = (sum(mids) / len(mids)) / (0.4 * best_k * math.sqrt(tau))
        if not hs_all:
            continue
        hs_all.sort()
        dep_all.sort()
        out.append({
            "date": date_str,
            "underlying": u,
            "dte": dte,
            "n_strikes_21d": len(strikes),
            "atm_strike": best_k if best_k is not None else float("nan"),
            "atm_rel_hspread": atm_hs,
            "atm_hspread_vol_pts": atm_vp,
            "atm_iv": atm_iv,
            "atm_depth": atm_dep,
            "med_rel_hspread_21d": hs_all[len(hs_all) // 2],
            "med_depth_21d": dep_all[len(dep_all) // 2],
            "n_contracts_total": totals.get(u, 0),
        })
    return out


FIELDS = ["date", "underlying", "dte", "n_strikes_21d", "atm_strike",
          "atm_rel_hspread", "atm_hspread_vol_pts", "atm_iv", "atm_depth",
          "med_rel_hspread_21d", "med_depth_21d", "n_contracts_total"]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hive", default="C:/atx-data/opra-hive")
    ap.add_argument("--dates", required=True, help="comma-separated ISO dates")
    ap.add_argument("--out", required=True)
    ap.add_argument("--dte-lo", type=int, default=10)
    ap.add_argument("--dte-hi", type=int, default=45)
    a = ap.parse_args(argv)

    rows = []
    for d in a.dates.split(","):
        d = d.strip()
        p = os.path.join(a.hive, f"date={d}", "data.parquet")
        if not os.path.exists(p):
            print(f"SKIP {d}: no partition", file=sys.stderr)
            continue
        r = process_date(p, d, a.dte_lo, a.dte_hi)
        print(f"{d}: {len(r)} underliers", file=sys.stderr)
        rows.extend(r)

    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(FIELDS) + "\n")
        for r in rows:
            f.write("\t".join(
                (f"{r[k]:.8g}" if isinstance(r[k], float) and not math.isnan(r[k])
                 else ("nan" if isinstance(r[k], float) else str(r[k])))
                for k in FIELDS) + "\n")
    print(f"wrote {len(rows)} rows -> {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
