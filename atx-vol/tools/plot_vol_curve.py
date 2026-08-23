#!/usr/bin/env python3
"""Plot one expiry's curve — in vol space or price space — against the market.

Reads an `atx-vol-chain-export` slice and draws our fitted values through the
quoted bid-ask band, so "are we pricing inside the market" is a question you
answer by looking rather than by trusting a summary statistic.

    # vol space, the OTM smile
    python atx-vol/tools/plot_vol_curve.py --chain <chain.parquet> \
        --symbol KMX --expiry 2026-09 --out kmx_sep26_vol.png

    # price space, every put — the sanity check
    python atx-vol/tools/plot_vol_curve.py --chain <chain.parquet> \
        --symbol KMX --expiry 2026-09 --space price --side put \
        --out kmx_sep26_put_px.png

VOL SPACE inverts each quote to an American implied vol at the row's OWN
recorded pricing inputs (`uPrc`, `rate`, `sdiv`, `years`), so a market vol is
directly comparable to the `srVol` beside it. Quotes that no volatility can
reproduce -- a deep-ITM bid below intrinsic, a zero bid -- have no vol and are
reported as such rather than coerced to a number.

PRICE SPACE needs no inversion and so has no such gaps: every quote is drawn.
That is exactly why it is the better sanity check. It gets two panels:

  * top, price against strike on a LOG axis. Linear would let the deep-ITM
    contracts (tens of dollars) flatten the whole OTM wing (cents) into the
    x-axis, which is where a mispricing would actually hide.
  * bottom, (fair - mid) measured in HALF-SPREADS. This is the panel to read.
    +-1 is the quote itself, so a point inside that band is a fair value the
    market cannot immediately trade against, and the distance is in the only
    units that mean the same thing at a $0.05 strike and a $40 one. A raw
    dollar error does not.

`--side` picks the leg. `otm` (the default) keeps puts below the forward and
calls above -- the ITM wing of each side is the same information as the OTM
wing of the other, through a wider spread and a bigger early-exercise
correction, so drawing both plots two noisy copies of one smile. For a price
sanity check you usually want a whole leg: `--side put` or `--side call`.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

# atxvol MUST initialise before pyarrow (they collide over arrow.dll on Windows);
# importing atxvol.chain guarantees it. See atxvol/chain.py's module docstring.
from atxvol import chain

import matplotlib

matplotlib.use("Agg")  # headless: this writes a file, it never opens a window
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

BID_C, ASK_C, FAIR_C, MID_C = "#C0392B", "#1E8449", "#1B2A41", "#B7791F"
BAND_C = "#5B8DEF"


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--chain", type=pathlib.Path, required=True,
                    help="parquet written by atx-vol-chain-export")
    ap.add_argument("--symbol", required=True, help="OSI root, e.g. KMX")
    ap.add_argument("--expiry", required=True,
                    help="'YYYY-MM-DD', or 'YYYY-MM' when the month lists exactly one")
    ap.add_argument("--out", type=pathlib.Path, required=True, help="output image")
    ap.add_argument("--space", choices=("vol", "price"), default="vol")
    ap.add_argument("--side", choices=("otm", "put", "call", "all"), default="otm",
                    help="which leg to draw (default otm: puts below F, calls above)")
    ap.add_argument("--x", choices=("strike", "logm", "znorm"), default="strike",
                    help="x axis: strike, log-moneyness ln(K/F), or normalized "
                         "strike z = ln(K/F)/(sigma*sqrt(T))")
    ap.add_argument("--z-window", type=float, default=0.0,
                    help="keep only |z| <= this, z = ln(K/F)/(sigma*sqrt(T)) with "
                         "sigma the slice's ATM fitted vol. 0 (default) = no window. "
                         "Tenor-independent, so 2 means the same on a 28-day slice "
                         "as on a 2-year one.")
    ap.add_argument("--all-strikes", action="store_true",
                    help="vol space: draw one-sided strikes too (default two-sided only)")
    ap.add_argument("--title", default="")
    return ap


def _select_side(frame, side, fwd):
    is_call = frame["okey_cp"].str.startswith("C")
    if side == "otm":
        return chain.otm_only(frame, fwd)
    if side == "put":
        return frame[~is_call].reset_index(drop=True)
    if side == "call":
        return frame[is_call].reset_index(drop=True)
    return frame.reset_index(drop=True)


def _plot_vol(rows, x, args, fwd):
    fig, ax = plt.subplots(figsize=(11, 6.5))
    bid = rows["bid_iv"].to_numpy(float)
    ask = rows["ask_iv"].to_numpy(float)
    fair = rows["srVol"].to_numpy(float)
    is_call = rows["okey_cp"].str.startswith("C").to_numpy()

    band = np.isfinite(bid) & np.isfinite(ask)
    if band.any():
        ax.fill_between(x[band], bid[band], ask[band], alpha=0.20, color=BAND_C,
                        linewidth=0, label="market bid-ask vol band")
    ax.plot(x[np.isfinite(bid)], bid[np.isfinite(bid)], "v", markersize=5,
            color=BID_C, label="market bid vol")
    ax.plot(x[np.isfinite(ask)], ask[np.isfinite(ask)], "^", markersize=5,
            color=ASK_C, label="market ask vol")
    good = np.isfinite(fair)
    ax.plot(x[good], fair[good], color=FAIR_C, linewidth=2.0, label="atx-vol fair vol")
    for mask, marker, lab in ((is_call & good, "o", "fair vol (call leg)"),
                              (~is_call & good, "s", "fair vol (put leg)")):
        if mask.any():
            ax.plot(x[mask], fair[mask], marker, markersize=4.5, color=FAIR_C,
                    markerfacecolor="white", label=lab)
    ax.set_ylabel("implied volatility")
    return fig, ax


def _plot_price(rows, x, args, fwd):
    """Two panels: prices on a log axis, and the fair-vs-mid error in half-spreads."""
    fig, (ax, ax2) = plt.subplots(
        2, 1, figsize=(11, 8.4), sharex=True,
        gridspec_kw={"height_ratios": [2.6, 1.0], "hspace": 0.08})

    bid = rows["bidPrc"].to_numpy(float)
    ask = rows["askPrc"].to_numpy(float)
    fair = rows["srPrc"].to_numpy(float)
    two = (bid > 0) & (ask > 0)
    mid = np.where(two, (bid + ask) / 2.0, np.nan)

    ax.fill_between(x[two], bid[two], ask[two], alpha=0.20, color=BAND_C,
                    linewidth=0, label="market bid-ask")
    ax.plot(x[bid > 0], bid[bid > 0], "v", markersize=5, color=BID_C, label="bid")
    ax.plot(x[ask > 0], ask[ask > 0], "^", markersize=5, color=ASK_C, label="ask")
    ax.plot(x[two], mid[two], "--", linewidth=1.1, color=MID_C, label="mid")
    good = np.isfinite(fair)
    ax.plot(x[good], fair[good], "-o", linewidth=1.8, markersize=4,
            color=FAIR_C, markerfacecolor="white", label="atx-vol fair value")
    # Log: a linear axis lets the deep-ITM contracts flatten the OTM wing into
    # the x-axis, which is precisely where a mispricing would hide.
    ax.set_yscale("log")
    ax.set_ylabel("option price (log scale)")

    # Half-spreads from mid. +-1 IS the quote, so this is the panel that answers
    # "are we inside the market" in units that mean the same thing at every strike.
    half = np.where(two, (ask - bid) / 2.0, np.nan)
    err = np.where(two & (half > 0), (fair - mid) / np.where(half > 0, half, np.nan), np.nan)
    ok = np.isfinite(err)
    inside = ok & (np.abs(err) <= 1.0)
    ax2.axhspan(-1, 1, color=BAND_C, alpha=0.18, linewidth=0)
    ax2.axhline(0.0, color=MID_C, linestyle="--", linewidth=1.0)
    ax2.plot(x[inside], err[inside], "o", markersize=5, color=FAIR_C,
             markerfacecolor="white", label="inside the quote")
    out = ok & ~inside
    if out.any():
        ax2.plot(x[out], err[out], "o", markersize=6, color=BID_C,
                 label="outside the quote")
    ax2.set_ylabel("(fair − mid)\nin half-spreads")
    ax2.grid(alpha=0.25, linewidth=0.6)
    ax2.legend(frameon=False, fontsize=8, loc="best")
    return fig, ax, ax2, err, ok, inside


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    frame = chain.load_chain(args.chain, symbol=args.symbol)
    if frame.empty:
        print(f"{args.symbol}: not present in {args.chain}", file=sys.stderr)
        return 3
    sl = chain.slice_expiry(frame, args.expiry)
    fwd, spot, years = (chain.forward(sl), float(sl["uPrc"].iloc[0]),
                        float(sl["years"].iloc[0]))
    iv = chain.add_market_ivs(sl)
    # Reference vol from the WHOLE slice before any filtering, so the normalised
    # axis does not move when --side or --z-window changes what is drawn.
    ref_vol = chain.atm_vol(iv, fwd)
    rows = _select_side(iv, args.side, fwd)
    n_slice = len(rows)
    if args.z_window > 0.0:
        rows = chain.within_z(rows, args.z_window, fwd, ref_vol)
        if rows.empty:
            print(f"{args.symbol} {args.expiry}: no strike inside |z| <= "
                  f"{args.z_window} (sigma={ref_vol:.4f})", file=sys.stderr)
            return 3

    n_all = len(rows)
    two_sided = (rows["bidPrc"] > 0) & (rows["askPrc"] > 0)
    n_one_sided = int((~two_sided).sum())
    n_below = int((rows["bid_iv_reason"] == "below_intrinsic").sum())
    # Vol space cannot draw a strike it could not invert; price space always can,
    # so it never filters. That asymmetry is the reason price space is the better
    # sanity check, and the footer says so per-plot.
    if args.space == "vol" and not args.all_strikes:
        rows = rows[two_sided].reset_index(drop=True)
    if rows.empty:
        print(f"{args.symbol} {args.expiry}: nothing survived the filters "
              f"({n_all} in the slice)", file=sys.stderr)
        return 3

    if args.x == "znorm":
        x = chain.normalized_strike(rows, fwd, ref_vol)
    elif args.x == "logm":
        x = np.log(rows["okey_xx"] / fwd).to_numpy(float)
    else:
        x = rows["okey_xx"].to_numpy(float)
    order = np.argsort(x)
    rows, x = rows.iloc[order].reset_index(drop=True), x[order]

    err = ok = inside = None
    if args.space == "vol":
        fig, ax = _plot_vol(rows, x, args, fwd)
        axes_bottom = ax
    else:
        fig, ax, axes_bottom, err, ok, inside = _plot_price(rows, x, args, fwd)

    anchor = fwd if args.x == "strike" else 0.0
    for a in {ax, axes_bottom}:
        a.axvline(anchor, color="#7F8C8D", linestyle="--", linewidth=1.0)
    axes_bottom.set_xlabel({"logm": "log-moneyness  ln(K/F)",
                            "znorm": "normalized strike  z = ln(K/F) / (σ√T)",
                            "strike": "strike"}[args.x])

    resolved = (f"{int(sl['okey_yr'].iloc[0]):04d}-{int(sl['okey_mn'].iloc[0]):02d}-"
                f"{int(sl['okey_dy'].iloc[0]):02d}")
    leg = {"otm": "OTM leg", "put": "puts", "call": "calls", "all": "both legs"}[args.side]
    window = f"   |z|≤{args.z_window:g}" if args.z_window > 0.0 else ""
    ax.set_title(args.title or (f"{args.symbol}  {resolved}  ({leg}{window})   "
                                f"spot {spot:.2f}   forward {fwd:.2f}   "
                                f"{years * 365:.0f}d   σatm {ref_vol:.3f}"), fontsize=12)
    ax.grid(alpha=0.25, linewidth=0.6)
    ax.legend(frameon=False, fontsize=9, loc="best")

    if args.space == "price":
        n_cmp = int(ok.sum())
        n_in = int(inside.sum())
        footer = (f"{n_all} of {n_slice} strikes in window   |   {n_one_sided} one-sided"
                  f"   |   fair inside the quote on {n_in}/{n_cmp} two-sided strikes")
    else:
        footer = (f"{len(rows)} of {n_slice} strikes drawn   |   {n_one_sided} one-sided"
                  f"   |   {n_below} bids below intrinsic (no bid vol exists)")
    fig.text(0.01, 0.01, footer, fontsize=8, color="#5D6D7E")
    # `tight_layout` cannot handle the shared-x gridspec the price panels use and
    # warns that the result "might be incorrect"; place them explicitly instead.
    if args.space == "price":
        fig.subplots_adjust(left=0.10, right=0.98, top=0.94, bottom=0.10)
    else:
        fig.tight_layout(rect=(0, 0.03, 1, 1))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=150)
    plt.close(fig)

    print(f"{args.symbol} {resolved} [{args.space}/{args.side}]: spot={spot:.2f} "
          f"forward={fwd:.4f} dte={years * 365:.1f}d  strikes={n_all}")
    if args.space == "price":
        print(f"  fair inside the quoted bid-ask on {int(inside.sum())}/{int(ok.sum())} "
              f"two-sided strikes")
        if ok.any():
            print(f"  |fair-mid| in half-spreads: median={np.nanmedian(np.abs(err[ok])):.3f} "
                  f"p90={np.nanpercentile(np.abs(err[ok]), 90):.3f} "
                  f"max={np.nanmax(np.abs(err[ok])):.3f}")
    print(f"  wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
