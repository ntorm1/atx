#!/usr/bin/env python3
"""Turn the underlier NBBO hive into an `ATX_CORPUS_SPOTS` overlay for the fitter.

WHY THIS EXISTS. `atx-vol-surface-db-build` implies each board's spot from
put-call parity on that board's OWN quotes. That works wherever the chain is
liquid and fails exactly where it is least affordable: a board with no strike
carrying a two-sided call AND a two-sided put on any expiry cannot imply a spot
at all, so it refuses to LOAD -- 943 of 6,189 underliers on the 2026-08-21
full-OPRA board (15%). Those cells never reach the fitter, so they appear in
neither `cells_ok` nor `cells_failed` and no `failed_cell` line names them; they
are only visible as the `n_load_errors` count. 626 of the 943 (66.4%) do have an
equity NBBO in `C:/atx-data/underlier-hive`. This writes that NBBO out in the
one artifact shape the loader already consumes, and `--spots` feeds it in.

WHAT IS AND IS NOT EMITTED. The usability rule is COPIED from the C++ side
(`ce::resolve_underlier`, atx-vol/tools/chain_export.hpp) so the two agree about
what a quote means:

  * both sides finite, `bid > 0`, `ask >= bid`  -- else no row. A one-sided or
    crossed quote is present-but-says-nothing, and a spot of 0 is not "no
    opinion" to the loader, it is a malformed row.
  * cash-settled index roots (SPX, NDX, VIX, ...) are EXCLUDED by list, not by
    inference. Their underlier is an index level that no equity NBBO feed
    carries, so a row for one would be a different instrument wearing the same
    ticker. For these the board's parity forward is the only spot there is.

A symbol with no row simply keeps its PCP-implied spot: the loader's
`MissingMarketInputPolicy::UseFallback` makes the overlay strictly additive, so
a partial file is legitimate and this tool never has to cover the board.

DO NOT OVERLAY A BOARD THAT CAN ALREADY IMPLY ITS OWN SPOT -- USE --only-no-pcp.
Measured A/B on the 2026-08-21 full-OPRA board, blanket overlay against no
overlay: `n_load_errors` fell 943 -> 364 exactly as intended, and coverage went
DOWN, 3,117 -> 3,022 served. 208 names that were served were lost and only 113
gained. The names lost are not marginal (AEE, ARCC, ARMK, AXTA); they had a
perfectly good parity spot and the overlay replaced it.

The reason is not a bug in the overlay, it is what the two spots MEAN. The
PCP-implied spot is the spot the options are actually priced off: it already
absorbs borrow, hard-to-borrow premium and the dividend the market is really
discounting, because it is solved FROM the option prices. The equity NBBO mid
is the spot the shares trade at, and the difference between them is a basis the
surface must then explain with skew it does not have. So the NBBO mid is
STRICTLY WORSE wherever parity works, and strictly better than nothing where it
does not. `--only-no-pcp` restricts the overlay to exactly that second set.

The mid is `(bid + ask) / 2` -- the same midpoint `resolve_underlier` forms, so
a board fitted through this overlay and a chain-export row for the same board
agree on `uPrc` instead of differing by the choice of spot.

Usage:
  python atx-vol/tools/make_spot_inputs.py \
      --underlier C:/atx-data/underlier-hive \
      --out C:/atx-data/universe/spots_2026-08.tsv \
      --from 2026-08-10 --to 2026-08-21 --snap-suffix T19:55:00Z

  # only the symbols a board actually lists (keeps the file to the real universe)
  python atx-vol/tools/make_spot_inputs.py --underlier <root> --out <tsv> \
      --from <d> --to <d> --board-root C:/atx-data/opra-all
"""

from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import sys

import pandas as pd
import pyarrow.parquet as pq

# The loader's fixed-point convention, shared with the OPRA hive: int64 1e-9
# dollars, with INT64_MIN meaning UNSET (not zero, not missing-and-harmless).
PX_SCALE = 1.0e-9
INT64_MIN = -(2 ** 63)

# OSI symbol layout, fixed width: root padded to 6, then YYMMDD, then C/P, then
# an 8-digit strike in thousandths. Slicing beats a regex on a 1.9M-row board.
OSI_EXP = slice(6, 12)
OSI_CP = 12
OSI_STRIKE = slice(13, 21)

# Copied verbatim from `kCashSettledIndexRoots` (atx-vol/tools/chain_export.hpp).
# A LIST, NOT AN INFERENCE: these are index series whose underlier is a level, so
# an equity NBBO row for one of them would be a different instrument. Keep in
# sync with the C++ array -- `--check-index-roots` asserts the two match.
CASH_SETTLED_INDEX_ROOTS = frozenset({
    "DJX", "MRUT", "MXEA", "MXEF", "NDX", "NDXP", "RUT",
    "RUTW", "SPX", "SPXW", "VIX", "XEO", "XND", "XSP",
})

MAGIC = "ATX_CORPUS_SPOTS\t1"
HEADER = "date\tsymbol\tspot\tsource\tas_of"
SOURCE = "equity_nbbo"


def dates_in(root: pathlib.Path, lo: str, hi: str) -> list[str]:
    out = []
    for p in sorted(root.glob("date=*")):
        d = p.name[len("date="):]
        if lo <= d <= hi:
            out.append(d)
    return out


def board_symbols(board_root: pathlib.Path, date: str) -> set[str] | None:
    """The underliers the OPRA board for `date` actually lists, or None.

    Restricting the overlay to these keeps the artifact the size of the real
    universe rather than the size of the equity tape (8,908 tickers on
    2026-08-21 against 6,189 underliers), and a spot for a symbol with no board
    is inert anyway -- the loader only ever looks a cell up by (date, symbol).
    """
    path = board_root / f"date={date}" / "data.parquet"
    if not path.exists():
        return None
    col = pq.ParquetFile(path).read(columns=["underlying"]).column("underlying")
    return set(col.to_pylist())


def can_imply_spot(board_root: pathlib.Path, date: str) -> set[str] | None:
    """Underliers whose board CAN imply a spot from put-call parity, or None.

    Reproduces the loader's own precondition rather than trusting a counter: a
    spot is implied from a strike carrying a two-sided CALL and a two-sided PUT
    on the SAME expiry, so one such pair anywhere on the board is enough. The
    derivation is validated -- it yields 942 non-implying names on 2026-08-21
    against the loader's own `n_load_errors` 943, and 896-970 per session across
    the ten sessions of 2026-08.
    """
    path = board_root / f"date={date}" / "data.parquet"
    if not path.exists():
        return None
    t = pq.ParquetFile(path).read(columns=["underlying", "symbol", "bid_px", "ask_px"])
    sym = t.column("symbol").to_pandas().str
    frame = pd.DataFrame({
        "underlying": t.column("underlying").to_pandas(),
        "exp": sym[OSI_EXP],
        "cp": sym[OSI_CP],
        "strike": sym[OSI_STRIKE],
        "bid": t.column("bid_px").to_pandas(),
        "ask": t.column("ask_px").to_pandas(),
    })
    two = (frame["bid"] != INT64_MIN) & (frame["ask"] != INT64_MIN) & (frame["bid"] > 0)
    tw = frame[two]
    pairs = tw.groupby(["underlying", "exp", "strike"])["cp"].agg(
        lambda s: "C" in set(s) and "P" in set(s))
    return set(pairs[pairs].index.get_level_values(0).unique())


def spot_rows(underlier_root: pathlib.Path, date: str, keep: set[str] | None):
    """Yield (symbol, mid) for every usable NBBO row, plus a rejection tally."""
    path = underlier_root / f"date={date}" / "underlier.parquet"
    if not path.exists():
        return [], {"no_file": 1}
    t = pq.ParquetFile(path).read(columns=["underlying", "bid_px", "ask_px"])
    tickers = t.column("underlying").to_pylist()
    bids = t.column("bid_px").to_pylist()
    asks = t.column("ask_px").to_pylist()

    rows, seen = [], set()
    tally = {"index_root": 0, "unset": 0, "nonpositive_bid": 0, "crossed": 0,
             "not_on_board": 0, "duplicate": 0}
    for tk, b, a in zip(tickers, bids, asks):
        if tk in CASH_SETTLED_INDEX_ROOTS:
            tally["index_root"] += 1
            continue
        if keep is not None and tk not in keep:
            tally["not_on_board"] += 1
            continue
        if b == INT64_MIN or a == INT64_MIN:
            tally["unset"] += 1
            continue
        bid, ask = b * PX_SCALE, a * PX_SCALE
        if not (bid > 0.0):
            tally["nonpositive_bid"] += 1
            continue
        if ask < bid:
            tally["crossed"] += 1
            continue
        if tk in seen:
            # `CorpusMarketInputTable::create` rejects a duplicate (date, symbol)
            # outright, so a second row here would poison the WHOLE file. Drop it
            # loudly here instead of shipping an artifact the loader refuses.
            tally["duplicate"] += 1
            continue
        seen.add(tk)
        rows.append((tk, 0.5 * (bid + ask)))
    return rows, tally


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--underlier", type=pathlib.Path, required=True,
                    help="underlier NBBO hive root holding date=<D>/underlier.parquet")
    ap.add_argument("--out", type=pathlib.Path, required=True, help="ATX_CORPUS_SPOTS TSV")
    ap.add_argument("--from", dest="lo", required=True)
    ap.add_argument("--to", dest="hi", required=True)
    ap.add_argument("--snap-suffix", default="T19:55:00Z",
                    help="stamp written to as_of; must not be LATER than the cell's "
                         "own date or CorpusMarketInputTable::create rejects the row")
    ap.add_argument("--board-root", type=pathlib.Path,
                    help="OPRA hive root; restricts the overlay to listed underliers")
    ap.add_argument("--only-no-pcp", action="store_true",
                    help="emit a spot ONLY for boards that cannot imply one from put-call "
                         "parity. STRONGLY RECOMMENDED -- see the module docstring: a blanket "
                         "overlay measured 3,117 -> 3,022 served on 2026-08-21, losing 208 "
                         "names to gain 113. Requires --board-root.")
    args = ap.parse_args()

    if args.only_no_pcp and not args.board_root:
        print("--only-no-pcp needs --board-root: the set of boards that cannot imply a "
              "spot is derived from the board itself", file=sys.stderr)
        return 2

    dates = dates_in(args.underlier, args.lo, args.hi)
    if not dates:
        print(f"no date= partitions in {args.underlier} within {args.lo}..{args.hi}",
              file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    total = 0
    with args.out.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write(MAGIC + "\n")
        fh.write(HEADER + "\n")
        for d in dates:
            keep = board_symbols(args.board_root, d) if args.board_root else None
            if args.only_no_pcp:
                implies = can_imply_spot(args.board_root, d) or set()
                # Intersect rather than replace: --board-root's "is it listed at
                # all" filter still applies, and a board that implies its own
                # spot is removed from the overlay entirely.
                keep = (keep or set()) - implies
            rows, tally = spot_rows(args.underlier, d, keep)
            as_of = d + args.snap_suffix
            for tk, mid in sorted(rows):
                fh.write(f"{d}\t{tk}\t{mid:.6f}\t{SOURCE}\t{as_of}\n")
            total += len(rows)
            drops = " ".join(f"{k}={v}" for k, v in tally.items() if v)
            print(f"{d}  spots={len(rows):,d}  {drops}")
    print(f"\n{args.out}  {total:,d} rows over {len(dates)} date(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
