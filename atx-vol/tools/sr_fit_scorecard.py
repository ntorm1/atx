"""Score our fitted surface against SpiderRock, in vol space and price space at once.

WHY THIS EXISTS. atx-vol-oracle-bench answers a different question. Its Mode A
prices each row at the vendor's OWN srVol, and Mode B inverts each row's own mid.
Both are per-row by deliberate design -- oracle_bench_main.cpp:617 says so
outright -- so neither can score a PARAMETRIC SURFACE fitted across strikes. That
is the quantity this file scores, and nothing else in the tree scores it.

It also reports a statistic the bench structurally cannot: a SIGNED one. The
bench's scorecard cells store |err| only, so a surface sitting uniformly rich and
a surface that is symmetrically noisy are indistinguishable there. Those are not
remotely the same defect, and telling them apart is the whole diagnostic value of
this tool -- so every band reports median signed bias beside MAE.

WHAT IS COMPARED. Both sides are parquet, joined on the contract:
  ours   -- an atx-vol-chain-export output. Its columns are NAMED for the vendor
            (srVol, srPrc, years) but the NUMBERS ARE OURS; the export writes a
            tblOptionIntradayHist-SHAPED file. That naming has misled before, so
            every column is renamed `our_*` at load and never appears unprefixed.
  vendor -- the licensed oracle store partition the fit was built from.

TWO SPACES, DELIBERATELY BOTH. Vol and price disagreement are not redundant views
of one error. A tau mismatch moves vol while leaving price nearly fixed (same
quote, different clock -> different vol); a carry or spot mismatch moves price
while leaving smile shape intact. Reporting one hides the other, so both print
side by side, and `years` is carried through the join purely so the clock itself
can be checked rather than inferred.

UNITS, fixed to match what they are compared against:
  vol    absolute vol basis points (decimal vol x 1e4) AND relative percent,
         because a clock error is multiplicative and an anchoring error is not --
         the two are separable only if both are shown.
  price  ticks at $0.01, directly comparable to the bench's mode_a_price_mae.
  tau    relative percent.

VOL MAE IN BASIS POINTS IS A TRAP, AND THE TOOL SAYS SO IN ITS OWN OUTPUT.
Measured on 126,920 contracts: vol MAE rises from 58 bp at |z|<1 to 2590 bp at
|z|>5, which reads like the wings are broken. They are not. The vendor's OWN
bid/ask IV width over those same bands rises from 217 bp to 10,762 bp, and the
RATIO of our error to that width is flat at 0.13-0.27 everywhere. Almost all of
the apparent wing blow-up is quote width -- regions where the market does not
determine vol to better than a hundred vol points, so neither surface can be
scored there in absolute units.

So the headline statistics here are SCALE-FREE:
  contain%    share of contracts whose our_vol lies inside the vendor's own
              [bidIV, askIV]. This is the honest target: a surface inside the
              quoted band is not disagreeing with the market at all.
  srcontain%  the SAME statistic for the vendor's own srVol. It is the CEILING
              -- what a surface built by the people who published these quotes
              achieves against them. Measured at 98.7-99.4%, so it is a real
              bar and not an unreachable ideal. Reporting our number without it
              would make 83% look like either success or failure at will.
  nerr        median |our_vol - sr_vol| / (askIV - bidIV): disagreement in units
              of how much the market itself is unsure. Comparable across
              moneyness and tenor in a way basis points are not.

THE QUOTED BAND IS THE DEFAULT POPULATION. Outside the vendor's two-sided quote
both surfaces are extrapolating, and a wing MAE measures which extrapolation you
prefer rather than which fit is better -- it moved one measured name from 132 bp
to 1378 bp. `--all-strikes` opts back into the full strip. Either way BOTH counts
print, so a restriction can never be mistaken for a result.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

import numpy as np
import polars as pl

MISSING = -99.0  # chain-export's "no value" sentinel, and the store's too

# Contract identity. Strike is rounded to 4dp on BOTH sides before joining: ours
# round-trips through an OSI milli-strike and the vendor's does not, so a raw
# float compare drops legs on 1e-9 dust.
KEY = ["okey_tk", "okey_yr", "okey_mn", "okey_dy", "okey_cp", "strike_k"]

DTE_BANDS = [(0, 7), (7, 30), (30, 90), (90, 365), (365, 100_000)]
Z_BANDS = [(-100.0, -2.0), (-2.0, -1.0), (-1.0, -0.25),
           (-0.25, 0.25), (0.25, 1.0), (1.0, 2.0), (2.0, 100.0)]


def _clean(expr: pl.Expr) -> pl.Expr:
    """Sentinel and non-finite to null, so neither can reach a mean."""
    return (pl.when(expr.is_null() | expr.is_nan() | (expr <= MISSING + 1e-9))
              .then(None).otherwise(expr))


def load_ours(path: pathlib.Path) -> pl.DataFrame:
    df = pl.read_parquet(path)
    return df.select([
        pl.col("okey_tk"),
        pl.col("okey_yr"), pl.col("okey_mn"), pl.col("okey_dy"),
        pl.col("okey_cp").str.slice(0, 1).str.to_uppercase().alias("okey_cp"),
        pl.col("okey_xx").round(4).alias("strike_k"),
        pl.col("okey_xx").alias("strike"),
        _clean(pl.col("srVol")).alias("our_vol"),
        _clean(pl.col("srPrc")).alias("our_prc"),
        _clean(pl.col("years")).alias("our_years"),
        _clean(pl.col("uPrc")).alias("our_uprc"),
    ])


def load_vendor(store: pathlib.Path, date: str, bucket: str,
                symbols: list[str] | None) -> pl.DataFrame:
    part = store / f"date={date}" / f"bucket_et={bucket}"
    files = sorted(part.glob("*.parquet"))
    if not files:
        raise FileNotFoundError(f"no parquet under {part}")
    lf = pl.concat([pl.scan_parquet(f) for f in files])
    if symbols:
        lf = lf.filter(pl.col("undSecKey_tk").is_in(symbols))
    return (lf.select([
        pl.col("okey_tk"),
        pl.col("okey_yr"), pl.col("okey_mn"), pl.col("okey_dy"),
        pl.col("okey_cp").str.slice(0, 1).str.to_uppercase().alias("okey_cp"),
        pl.col("okey_xx").round(4).alias("strike_k"),
        pl.col("undSecKey_tk").alias("underlier"),
        _clean(pl.col("srVol")).alias("sr_vol"),
        _clean(pl.col("srPrc")).alias("sr_prc"),
        _clean(pl.col("years")).alias("sr_years"),
        _clean(pl.col("uPrc")).alias("sr_uprc"),
        _clean(pl.col("bidIV")).alias("bid_iv"),
        _clean(pl.col("askIV")).alias("ask_iv"),
        _clean(pl.col("bidPrc")).alias("bid_prc"),
        _clean(pl.col("askPrc")).alias("ask_prc"),
    ]).unique(subset=KEY, keep="first").collect())


def add_moneyness(df: pl.DataFrame) -> pl.DataFrame:
    """z = ln(K/F) / (sigma_atm * sqrt(T)), per underlier x expiry.

    The reference sigma is the VENDOR's ATM vol, never ours. Using ours would let
    the band edges move whenever the fit moved, so any change that shifted vol
    would silently reshuffle which rows land in which band -- corrupting the very
    before/after comparison this tool exists to make.
    """
    grp = ["underlier", "okey_yr", "okey_mn", "okey_dy"]
    ref = (df.group_by(grp)
             .agg(pl.col("sr_uprc").median().alias("fwd"),
                  pl.col("sr_years").median().alias("t_ref")))
    df = df.join(ref, on=grp, how="left")
    atm = (df.filter(pl.col("sr_vol").is_not_null())
             .with_columns((pl.col("strike") - pl.col("fwd")).abs().alias("d"))
             .sort("d").group_by(grp)
             .agg(pl.col("sr_vol").first().alias("atm_vol")))
    df = df.join(atm, on=grp, how="left")
    denom = pl.col("atm_vol") * pl.col("t_ref").sqrt()
    return df.with_columns(
        pl.when((denom > 1e-9) & (pl.col("fwd") > 0) & (pl.col("strike") > 0))
          .then((pl.col("strike") / pl.col("fwd")).log() / denom)
          .otherwise(None).alias("z"),
        (pl.col("sr_years") * 365.0).alias("dte"))


def stats(sub: pl.DataFrame) -> dict:
    """One band's numbers. Signed medians lead, because bias is the point."""
    ok = sub.filter(pl.col("our_vol").is_not_null() & pl.col("sr_vol").is_not_null())
    out: dict = {"n": ok.height}
    if ok.height == 0:
        return out
    dv = (ok["our_vol"] - ok["sr_vol"]).to_numpy() * 1e4            # absolute vol bp
    rv = ((ok["our_vol"] / ok["sr_vol"]) - 1.0).to_numpy() * 100.0  # relative %
    out["vol_bias_bp"] = float(np.median(dv))
    out["vol_mae_bp"] = float(np.mean(np.abs(dv)))
    out["vol_p95_bp"] = float(np.percentile(np.abs(dv), 95))
    out["vol_bias_rel_pct"] = float(np.median(rv))

    okp = ok.filter(pl.col("our_prc").is_not_null() & pl.col("sr_prc").is_not_null())
    if okp.height:
        dp = (okp["our_prc"] - okp["sr_prc"]).to_numpy() * 100.0    # ticks at $0.01
        out["price_bias_tk"] = float(np.median(dp))
        out["price_mae_tk"] = float(np.mean(np.abs(dp)))
        out["price_n"] = okp.height

    okt = ok.filter(pl.col("our_years").is_not_null()
                    & pl.col("sr_years").is_not_null() & (pl.col("sr_years") > 0))
    if okt.height:
        rt = ((okt["our_years"] / okt["sr_years"]) - 1.0).to_numpy() * 100.0
        out["tau_bias_pct"] = float(np.median(rt))
        out["tau_absmax_pct"] = float(np.max(np.abs(rt)))

    # Scale-free block. `sr_contain_pct` is the ceiling, not a curiosity: it is
    # what the vendor's own surface scores against the vendor's own quotes, so it
    # says how much of any shortfall is actually reachable.
    okb = ok.filter(pl.col("bid_iv").is_not_null() & pl.col("ask_iv").is_not_null()
                    & (pl.col("ask_iv") > pl.col("bid_iv")))
    if okb.height:
        lo = okb["bid_iv"].to_numpy()
        hi = okb["ask_iv"].to_numpy()
        ov = okb["our_vol"].to_numpy()
        sv = okb["sr_vol"].to_numpy()
        out["contain_pct"] = float(100.0 * np.mean((ov >= lo) & (ov <= hi)))
        out["sr_contain_pct"] = float(100.0 * np.mean((sv >= lo) & (sv <= hi)))
        out["nerr"] = float(np.median(np.abs(ov - sv) / (hi - lo)))
        # Signed distance outside the band, in band-widths: 0 when contained.
        out_lo = np.maximum(lo - ov, 0.0)
        out_hi = np.maximum(ov - hi, 0.0)
        out["escape"] = float(np.mean((out_lo + out_hi) / (hi - lo)))
        out["contain_n"] = int(okb.height)
    return out


HDR = (f"{'band':<24}{'n':>7}{'contain%':>9}{'srcont%':>8}{'nerr':>7}{'escape':>8}"
       f"{'volbias':>9}{'volMAE':>8}{'rel%':>7}{'pxbias':>8}{'pxMAE':>8}{'tau%':>8}")


def line(name: str, s: dict) -> str:
    if not s.get("n"):
        return f"{name:<24}{0:>7}"
    nan = float("nan")
    return (f"{name:<24}{s['n']:>7}"
            f"{s.get('contain_pct', nan):>9.1f}{s.get('sr_contain_pct', nan):>8.1f}"
            f"{s.get('nerr', nan):>7.2f}{s.get('escape', nan):>8.2f}"
            f"{s['vol_bias_bp']:>+9.1f}{s['vol_mae_bp']:>8.1f}"
            f"{s['vol_bias_rel_pct']:>+7.2f}"
            f"{s.get('price_bias_tk', nan):>+8.2f}{s.get('price_mae_tk', nan):>8.2f}"
            f"{s.get('tau_bias_pct', nan):>+8.3f}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="score a fitted surface against SpiderRock")
    ap.add_argument("--chain", required=True, type=pathlib.Path,
                    help="atx-vol-chain-export output parquet (OUR numbers)")
    ap.add_argument("--store", type=pathlib.Path,
                    default=pathlib.Path("C:/atx-cache/oracle/spiderrock"))
    ap.add_argument("--date", required=True)
    ap.add_argument("--bucket", required=True, help="HHMM")
    ap.add_argument("--symbols", default="", help="comma list; empty = all in the chain")
    ap.add_argument("--all-strikes", action="store_true",
                    help="score the full strip, not just the vendor's two-sided band")
    ap.add_argument("--out", type=pathlib.Path, help="write a JSON receipt here")
    ap.add_argument("--label", default="", help="free-text tag carried into the receipt")
    a = ap.parse_args(argv)

    ours = load_ours(a.chain)
    syms = [s.strip().upper() for s in a.symbols.split(",") if s.strip()]
    vendor = load_vendor(a.store, a.date, a.bucket, syms or None)
    j = ours.join(vendor, on=KEY, how="inner")
    if j.height == 0:
        print("FATAL: join produced no rows -- key mismatch, not an empty result",
              file=sys.stderr)
        return 2
    j = add_moneyness(j)

    quoted = (pl.col("bid_iv").is_not_null() & pl.col("ask_iv").is_not_null()
              & (pl.col("bid_prc") > 0) & (pl.col("ask_prc") > pl.col("bid_prc")))
    n_all = j.height
    n_q = j.filter(quoted).height
    pop = j if a.all_strikes else j.filter(quoted)
    scope = "full strip" if a.all_strikes else "vendor two-sided band"

    print(f"chain rows {ours.height} | vendor rows {vendor.height} | joined {n_all} "
          f"| two-sided {n_q} ({100.0 * n_q / max(n_all, 1):.1f}%)")

    # RECONCILE WHAT WAS ASKED FOR AGAINST WHAT GOT SCORED. Every join here is
    # an INNER one, so a symbol that chain-export dropped, or that the store
    # does not carry, simply is not in `pop` -- and every number below is then a
    # real, correct figure for a population nobody chose. This is not
    # hypothetical: a 140-name run reported 130 names and read as complete,
    # because chain-export exits 0 when it drops a symbol it could not build.
    scored = set(pop['underlier'].unique().to_list())
    unscored = sorted(set(syms) - scored) if syms else []
    if unscored:
        print(f"WARNING: {len(unscored)} of {len(syms)} requested symbols scored "
              f"NO rows and are absent from every number below: "
              f"{', '.join(unscored)}", file=sys.stderr)

    print(f"scoring population: {scope} -> {pop.height} contracts\n")

    overall = stats(pop)
    print(HDR)
    print("-" * len(HDR))
    print(line("OVERALL", overall))
    print()

    bands: dict[str, list] = {"by_dte": [], "by_z": [], "by_underlier": []}
    for lo, hi in DTE_BANDS:
        s = stats(pop.filter((pl.col("dte") >= lo) & (pl.col("dte") < hi)))
        s["band"] = f"{lo}-{hi if hi < 99_999 else 'inf'}d"
        bands["by_dte"].append(s)
        print(line(f"  dte {s['band']}", s))
    print()
    for lo, hi in Z_BANDS:
        s = stats(pop.filter((pl.col("z") >= lo) & (pl.col("z") < hi)))
        s["band"] = f"[{lo:g},{hi:g})"
        bands["by_z"].append(s)
        print(line(f"  z {s['band']}", s))
    print()
    for u in sorted(pop.select("underlier").unique().to_series().to_list()):
        s = stats(pop.filter(pl.col("underlier") == u))
        s["band"] = u
        bands["by_underlier"].append(s)
        print(line(f"  {u}", s))

    if a.out:
        receipt = {"schema_version": 1, "kind": "sr_fit_scorecard",
                   "label": a.label, "date": a.date, "bucket_et": a.bucket,
                   "chain": str(a.chain), "scope": scope,
                   "rows_joined": n_all, "rows_two_sided": n_q,
            "symbols_requested": sorted(syms), "symbols_unscored": unscored,
                   "rows_scored": pop.height, "overall": overall, **bands}
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_text(json.dumps(receipt, indent=2))
        print(f"\nreceipt -> {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
