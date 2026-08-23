#!/usr/bin/env python3
"""2x2 grid: our fitted-surface fair vol against SpiderRock's srVol, one expiry.

`plot_vol_curve.py` draws ONE symbol and ONE fitted curve — and the `srVol`
column it draws is OURS, because `atx-vol-chain-export` writes a
tblOptionIntradayHist-SHAPED file whose numbers come from `PricerFitter` on our
board (chain_export_main.cpp:15-21). The vendor's own `srVol` lives in a
different store entirely, the oracle ingest at
`C:/atx-cache/oracle/spiderrock/date=<D>/bucket_et=<HHMM>/`. So a
vendor-vs-ours picture is a JOIN across two stores, which is what this does:

    python atx-vol/tools/plot_vol_grid.py \
        --chain C:/atx-cache/sr-chain/chain_20260814_1030.parquet \
        --store C:/atx-cache/oracle/spiderrock --date 2026-08-14 \
        --bucket-et 1030 --symbols KMX,MRNA,SPY,IBM --expiry 2026-09-18 \
        --out tmp/volcurve-1030-sep26.png

Each panel carries three things, and the third is what makes the other two
readable: the vendor's own quoted bid/ask IV band (`bidIV`/`askIV`, the vendor's
inversion of the same quotes), our fitted fair vol, and SpiderRock's `srVol`.
Two smooth curves through a band, not two curves in a vacuum — a gap between
them only matters relative to what the market was willing to trade.

`--side otm` (the default) keeps puts below the forward and calls above. The ITM
wing of each side is the same information through a wider spread, so drawing
both plots two noisy copies of one smile.

`--z-window` (default 2) cuts the strip at |ln(K/F)/(sigma*sqrt(T))| <= z. That
is where quotes exist: past it both curves are extrapolating and their
disagreement is a property of two extrapolation rules, not of the market. It is
also tenor- and name-independent, so the four panels are cut at the same place
in the only sense that means anything across a $60 name and a $780 one.

CAVEATS THIS PICTURE CANNOT DRAW, which any reading of it inherits:
  * SpiderRock's year fraction is a hybrid vol-time clock (trading hours
    weighted 0.7/1890, non-trading 0.3/6870), not calendar time. Our tau is
    calendar. A level difference between the two curves is partly that clock.
  * A vendor "bucket" is a per-contract last-quote stamp, not a synchronised
    cross-section: the 2026-08-14 10:30 bucket's own stamps span 14:30:32.99 to
    14:31:03.42 UTC. Both curves are built from those same scattered quotes, so
    the comparison is fair, but neither is an instant.
  * The board we fit reaches the fitter through the OPRA hive v2 schema, which
    has nowhere to carry the vendor's `rate`/`sdiv`/`ddiv`. We refit carry from
    put-call parity under a flat `--r`; SpiderRock did not.
"""

from __future__ import annotations

# atxvol MUST initialise before pyarrow/pandas (they collide over arrow.dll on
# Windows); importing atxvol.chain guarantees it. Never let an import sorter
# move this. See atxvol/chain.py's module docstring.
from atxvol import chain

import argparse  # noqa: E402
import pathlib  # noqa: E402
import sys  # noqa: E402

import matplotlib  # noqa: E402

matplotlib.use("Agg")  # headless: this writes a file, it never opens a window
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
import polars as pl  # noqa: E402

# House palette (tools/render_strangle_vs_varswap.py:76-84, tools/tearsheet.py:29-32).
INK, MUTE, GRID, PAPER = "#1b1b2f", "#6b7280", "#e6e6ea", "#ffffff"
OURS_C, VENDOR_C, BAND_C = "#0f766e", "#b4232a", "#5B8DEF"

# The contract identity both stores agree on. `okey_xx` is rounded before the
# join: it is a float on both sides and a strike is a thousandth of a dollar at
# worst, so an exact-float join is a silent-miss waiting to happen.
KEY = ["okey_tk", "okey_yr", "okey_mn", "okey_dy", "okey_xx_k", "okey_cp"]


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chain", type=pathlib.Path, required=True,
                    help="parquet written by atx-vol-chain-export (OUR fit)")
    ap.add_argument("--store", type=pathlib.Path,
                    default=pathlib.Path(r"C:\atx-cache\oracle\spiderrock"),
                    help="SpiderRock oracle store root (the VENDOR's srVol)")
    ap.add_argument("--date", required=True, help="YYYY-MM-DD trading session")
    ap.add_argument("--bucket-et", required=True, help="HHMM, e.g. 1030")
    ap.add_argument("--symbols", required=True,
                    help="comma-joined, in panel order (row-major)")
    ap.add_argument("--expiry", required=True,
                    help="'YYYY-MM-DD', or 'YYYY-MM' when the month lists exactly one")
    ap.add_argument("--side", choices=("otm", "put", "call", "all"), default="otm")
    ap.add_argument("--z-window", type=float, default=2.0,
                    help="keep only |z| <= this, z = ln(K/F)/(sigma*sqrt(T)) with sigma "
                         "the slice's ATM fitted vol. Tenor-independent, so 2 means the "
                         "same on a 35-day slice as on a 2-year one. 0 = no window.")
    ap.add_argument("--out", type=pathlib.Path, required=True, help="output PNG")
    return ap


def load_vendor(store: pathlib.Path, date: str, bucket_et: str,
                symbols: list[str]) -> pd.DataFrame:
    """The vendor's srVol and its own quoted IV band, for one bucket."""
    part = store / f"date={date}" / f"bucket_et={bucket_et}"
    files = sorted(part.glob("*.parquet"))
    if not files:
        raise SystemExit(f"no parquet under {part}")
    frame = (pl.scan_parquet(files)
               .filter(pl.col("undSecKey_tk").is_in(symbols))
               .select(["okey_tk", "okey_yr", "okey_mn", "okey_dy", "okey_xx",
                        "okey_cp", "srVol", "bidIV", "askIV", "uPrc", "years"])
               .collect()
               .to_pandas())
    frame["okey_xx_k"] = frame["okey_xx"].round(4)
    return frame.rename(columns={"srVol": "sr_vol", "uPrc": "sr_uprc",
                                 "years": "sr_years"}).drop(columns=["okey_xx"])


def panel_frame(chain_path: pathlib.Path, vendor: pd.DataFrame, symbol: str,
                expiry: str, side: str, z_window: float) -> tuple[pd.DataFrame, float]:
    """One symbol's OTM slice with both curves joined on the contract key."""
    ours = chain.load_chain(chain_path, symbol=symbol)
    if ours.empty:
        raise KeyError(f"{symbol}: no rows in the chain export")
    sl = chain.slice_expiry(ours, expiry)
    fwd = chain.forward(sl)
    # Forward and reference vol are taken from the WHOLE slice, before any side
    # or window filter, so the normalisation is a property of the smile and not
    # of whichever subset is being drawn. Otherwise `--side put` and
    # `--side call` would window against two different sigmas.
    ref_vol = chain.atm_vol(sl, fwd)
    if side == "otm":
        sl = chain.otm_only(sl, fwd)
    elif side in ("put", "call"):
        is_call = sl["okey_cp"].str.startswith("C")
        sl = sl[is_call if side == "call" else ~is_call]
    if z_window > 0.0:
        sl = chain.within_z(sl, z_window, fwd, ref_vol)
        if sl.empty:
            raise ValueError(f"no strike within |z| <= {z_window:g}")
    sl = sl.copy()
    sl["okey_xx_k"] = sl["okey_xx"].round(4)
    # The exporter writes -99 as a real float where it has no value; a -99 that
    # reaches a plot as a vol is worse than a gap.
    sl["our_vol"] = sl["srVol"].mask(sl["srVol"] == chain.MISSING)
    merged = sl.merge(vendor, on=KEY, how="left", validate="one_to_one")
    return merged.sort_values("okey_xx", kind="stable").reset_index(drop=True), fwd


def style_axis(ax) -> None:
    """render_strangle_vs_varswap.py:305-313, verbatim in spirit."""
    ax.set_facecolor(PAPER)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    for spine in ("left", "bottom"):
        ax.spines[spine].set_color(GRID)
    ax.grid(True, color=GRID, linewidth=0.7, alpha=0.9)
    ax.set_axisbelow(True)
    ax.tick_params(colors=MUTE, labelsize=7.5, length=3, width=0.7)


def draw_panel(ax, rows: pd.DataFrame, symbol: str, fwd: float) -> str:
    """One symbol's panel. Returns the one-line stat printed to stdout."""
    style_axis(ax)
    k = rows["okey_xx"].to_numpy(dtype=float)

    band = rows["bidIV"].notna() & rows["askIV"].notna()
    if band.any():
        ax.fill_between(k[band.to_numpy()],
                        rows.loc[band, "bidIV"].to_numpy() * 100.0,
                        rows.loc[band, "askIV"].to_numpy() * 100.0,
                        color=BAND_C, alpha=0.18, linewidth=0,
                        label="vendor quoted bid-ask IV", zorder=1)

    v = rows["sr_vol"].notna()
    ax.plot(k[v.to_numpy()], rows.loc[v, "sr_vol"].to_numpy() * 100.0,
            color=VENDOR_C, linewidth=1.4, marker="o", markersize=2.6,
            label="SpiderRock srVol", zorder=3)

    o = rows["our_vol"].notna()
    ax.plot(k[o.to_numpy()], rows.loc[o, "our_vol"].to_numpy() * 100.0,
            color=OURS_C, linewidth=1.4, marker="s", markersize=2.6,
            label="atx-vol fair vol", zorder=4)

    ax.axvline(fwd, color=MUTE, linewidth=0.8, linestyle=(0, (4, 3)), zorder=2)
    ax.annotate(f"F {fwd:,.2f}", xy=(fwd, 1.0), xycoords=("data", "axes fraction"),
                xytext=(3, -10), textcoords="offset points",
                fontsize=6.5, color=MUTE, ha="left", va="top")

    # TWO statistics, because one of them would lie. Restricted to strikes the
    # vendor actually quoted two-sided, the number says "do the two agree where
    # a market exists". Over every listed strike it also carries the wings,
    # where both curves are extrapolating past the last quote and a disagreement
    # costs nobody anything. The wing number is the larger one, always, and
    # quoting it alone would make a good fit look broken.
    both = o & v
    quoted = both & band
    n, nq = int(both.sum()), int(quoted.sum())
    if not n:
        stat = "no contract carries both vols"
    else:
        d = (rows.loc[both, "our_vol"].to_numpy() - rows.loc[both, "sr_vol"].to_numpy()) * 1e4
        if nq == n:  # every drawn strike is quoted; the second statistic is the first
            stat = f"n={n}  median {np.median(d):+.0f} bp  MAE {np.mean(np.abs(d)):.0f} bp"
        else:
            dq = (rows.loc[quoted, "our_vol"].to_numpy()
                  - rows.loc[quoted, "sr_vol"].to_numpy()) * 1e4
            stat = (f"quoted {nq}: median {np.median(dq):+.0f} bp, "
                    f"MAE {np.mean(np.abs(dq)):.0f} bp   ·   "
                    f"all {n}: MAE {np.mean(np.abs(d)):.0f} bp")
    ax.set_title(f"{symbol}    {stat}", fontsize=8.4, fontweight="bold",
                 color=INK, loc="left", pad=5)
    ax.set_xlabel("strike", fontsize=7.5, color=MUTE)
    ax.set_ylabel("implied vol (%)", fontsize=7.5, color=MUTE)
    n_missing = int(o.sum() - both.sum())
    n_flat = int(rows.loc[o, "our_vol"].round(6).duplicated(keep=False).sum())
    notes = []
    if n_missing:
        notes.append(f"{n_missing} of ours unmatched in the vendor store")
    if n_flat > 0.3 * int(o.sum()):
        notes.append(f"{n_flat} of our {int(o.sum())} vols are flat-repeated "
                     "— a mark-surface interpolant, not a fitted risk surface")
    return f"{symbol}: {stat}" + ("  (" + "; ".join(notes) + ")" if notes else "")


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    symbols = [s.strip() for s in args.symbols.split(",") if s.strip()]
    if len(symbols) != 4:
        raise SystemExit(f"--symbols must name exactly 4 for a 2x2 grid, got {len(symbols)}")

    vendor = load_vendor(args.store, args.date, args.bucket_et, symbols)

    fig = plt.figure(figsize=(12.4, 8.6), dpi=150, facecolor=PAPER)
    gs = fig.add_gridspec(2, 2, hspace=0.34, wspace=0.20,
                          left=0.065, right=0.985, top=0.865, bottom=0.085)

    lines = []
    handles = None
    for i, symbol in enumerate(symbols):
        ax = fig.add_subplot(gs[i // 2, i % 2])
        try:
            rows, fwd = panel_frame(args.chain, vendor, symbol, args.expiry,
                                    args.side, args.z_window)
        except (KeyError, ValueError) as exc:
            style_axis(ax)
            ax.text(0.5, 0.5, f"{symbol}\n{exc}", transform=ax.transAxes,
                    ha="center", va="center", fontsize=8, color=VENDOR_C, wrap=True)
            ax.set_title(symbol, fontsize=9.0, fontweight="bold", color=INK,
                         loc="left", pad=5)
            lines.append(f"{symbol}: FAILED — {exc}")
            continue
        lines.append(draw_panel(ax, rows, symbol, fwd))
        if handles is None:
            handles = ax.get_legend_handles_labels()

    if handles is not None:
        fig.legend(*handles, loc="upper right", bbox_to_anchor=(0.985, 0.938),
                   frameon=False, fontsize=8, ncols=3)

    window = (f", |z| <= {args.z_window:g}" if args.z_window > 0.0 else "")
    fig.suptitle(f"Fitted fair vol vs SpiderRock srVol — {args.expiry} expiry, "
                 f"{args.side.upper()} only{window}",
                 x=0.065, y=0.965, ha="left", fontsize=13.5,
                 fontweight="bold", color=INK)
    fig.text(0.065, 0.928,
             f"{args.date}  {args.bucket_et[:2]}:{args.bucket_et[2:]} ET slice   ·   "
             f"ours: atx-vol-chain-export refit of the vendor board   ·   "
             f"vendor: tblOptionIntradayHist",
             ha="left", fontsize=8.5, color=MUTE)
    fig.text(0.985, 0.020,
             "Vendor tau is a hybrid vol-time clock, ours is calendar; a vendor bucket is a "
             "per-contract last-quote stamp, not an instant; our carry is refit from parity "
             "under a flat r, the vendor's is not.",
             ha="right", va="bottom", fontsize=6.8, color=MUTE, style="italic")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, facecolor=PAPER)
    plt.close(fig)
    for line in lines:
        print(line)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
