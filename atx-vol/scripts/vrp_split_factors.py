#!/usr/bin/env python3
"""Emit the `vrp_panel --splits` reference TSV from the atx-db warehouse.

This is a REFERENCE-DATA GENERATOR, not part of the panel QA tier. It reads
the atx-db DuckDB warehouse READ-ONLY and writes the three-column grammar
`load_vrp_split_factors` (atx-vol/src/analytics/vrp_panel.hpp) contracts on:

    # ...provenance comments...
    symbol<TAB>ex_date<TAB>price_factor<TAB>...ignored provenance columns...

`price_factor` is the multiplier applied to every session STRICTLY BEFORE
`ex_date`, i.e. 0.1 for a 10:1 forward split. That is exactly the warehouse's
own `equity_daily_bars.split_factor` convention (0.5 on a 2:1), so this script
copies the vendor factor through unchanged -- it never derives a ratio from
observed prices, and it has no detection threshold of its own.

## Why a band, and why THIS band

`equity_daily_bars.split_factor` carries BOTH split and cash-dividend price
adjustments in one column (BKNG's quarterly dividends appear as ~0.9977).
Dividend factors must NOT be back-adjusted out of a volatility panel: a
dividend drop is a real price return that realized vol legitimately contains.
The split/dividend discriminator is NOT invented here -- it is atx-db's own
published `split_policy`, `included_factors: "split_factor <= 0.8 or >= 1.25"`
with `excluded: "small dividend adjustment factors"`
(atx-db/src/atx_db/earnings_acceleration.py:123 and :284, and the identical
predicate at earnings_surprise.py:164). This script reuses that band verbatim
so the panel and the warehouse's own factor pipelines agree by construction.
`--min-ratio` / `--max-ratio` expose it, but changing it forks that agreement.

## Why not `adjusted_close`

atx-db's own back-adjuster refuses that column and says why
(`equity_price_metrics.py:_back_adjusted_close`): in the cached sample it is
"an unadjusted lagged close and leaves split jumps in, which would inflate
returns/volatility". The factor column is the authority; the adjusted-price
column is not.

## Corroboration -- why the factor column alone is not enough

The vendor factor column carries FALSE POSITIVES: a declared factor parked on
the ANNOUNCEMENT date, where no price step occurred. Measured case, V 2015:
`split_factor=0.249546` on 2015-02-11 with close 265.99 against a prior 264.55
(no step at all), while the genuine 4:1 ex-date is 2015-03-19 (267.67 ->
66.81, factor 0.25). Emitting the announcement row would divide V's entire
prior history by four on top of the real split.

So each event is corroborated against the SECOND, INDEPENDENT column of the
same authoritative source -- the observed close step:

    residual = (prev_close / close) * price_factor      ~ 1.0 when the
                                                          factor explains
                                                          the step

This is a cross-check between two warehouse columns, NOT a price-based
detection rule: it can only ever REJECT a declared event, never invent one,
and the factor that survives is still the vendor's own number copied
unchanged. Measured over the SP100 universe (39 declared events, full
warehouse span): 37 genuine events span residual 0.888..1.030, the single
announcement artifact sits at 0.248, and one 2014 GOOGL event has no prior
bar to check. The default band [0.75, 1.3333] therefore clears the tightest
genuine event by 1.18x and the artifact by 3.0x, with nothing in between.

Rejected events are EXCLUDED from the data rows, listed in the file's `#`
provenance block, echoed to stderr, and counted in `n_rejected` -- never
dropped silently. An event whose prior close is unavailable cannot be checked;
it is EMITTED (refusing a real event is the worse error) and counted in
`n_uncorroborated` so a reader can see it was taken on trust.

## Dependency note

This is the ONE script in atx-vol/scripts/ that is not stdlib-only: reading a
DuckDB warehouse requires `duckdb`. It is a one-shot provenance tool whose
OUTPUT is checked in, so neither the panel build nor the QA tier ever imports
it. See atx-vol/scripts/README.md.

CLI:
    python vrp_split_factors.py --db <warehouse.duckdb> --out <factors.tsv>
        [--universe <symbols.txt|csv>] [--uid SYM]...
        [--from YYYY-MM-DD] [--to YYYY-MM-DD]
        [--min-ratio 0.8] [--max-ratio 1.25]

Exit codes: 0 wrote the file (even when zero events matched -- an empty
reference is a valid, meaningful answer), 2 bad args / unreadable warehouse.

Run: python -m pytest atx-vol/scripts/vrp_split_factors_test.py -q
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import sys
from pathlib import Path

# atx-db's published split_policy band (see the module docstring).
DEFAULT_MIN_RATIO = 0.8
DEFAULT_MAX_RATIO = 1.25

# Corroboration band on (prev_close/close)*price_factor. Justified against the
# measured SP100 residuals in the module docstring: genuine 0.888..1.030,
# announcement artifact 0.248.
DEFAULT_CORROBORATE_LO = 0.75
DEFAULT_CORROBORATE_HI = 4.0 / 3.0

BAND_SOURCE = (
    "atx-db split_policy: included_factors 'split_factor <= 0.8 or >= 1.25', "
    "excluded 'small dividend adjustment factors' "
    "(src/atx_db/earnings_acceleration.py:123,:284; earnings_surprise.py:164)"
)

_TABLE = "equity_daily_bars"


def _die(msg: str) -> None:
    print(f"vrp_split_factors: {msg}", file=sys.stderr)
    raise SystemExit(2)


def read_universe(path: Path) -> list[str]:
    """One symbol per line, or a CSV/TSV carrying a `symbol` column.

    Blank lines and `#` comments are skipped. Order is irrelevant (the emitted
    file is sorted), but duplicates are collapsed so a repeated membership row
    cannot widen the IN-list.
    """
    text = path.read_text(encoding="utf-8-sig")
    lines = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
    if not lines:
        return []
    head = lines[0]
    delim = "\t" if "\t" in head else ("," if "," in head else None)
    if delim is None:
        return sorted({ln.strip() for ln in lines})
    rows = list(csv.reader(lines, delimiter=delim))
    header = [c.strip().lower() for c in rows[0]]
    if "symbol" not in header:
        _die(f"--universe '{path}' is delimited but has no 'symbol' column")
    col = header.index("symbol")
    out = {r[col].strip() for r in rows[1:] if len(r) > col and r[col].strip()}
    return sorted(out)


def query_events(
    db: Path,
    symbols: list[str],
    date_from: str | None,
    date_to: str | None,
    min_ratio: float,
    max_ratio: float,
) -> list[dict[str, object]]:
    """Split events from the warehouse, sorted by (symbol, ex_date).

    Returns the vendor factor unchanged plus provenance: the ex-date close, the
    prior close, and the ratio those two IMPLY. The implied ratio is reported
    so a reader can see the vendor factor and the observed step agree; it is
    never used to derive or override the factor.
    """
    try:
        import duckdb  # noqa: PLC0415 -- optional dep, see module docstring
    except ImportError:  # pragma: no cover - environment-dependent
        _die("the 'duckdb' package is required (run under the atx-db venv)")

    if not db.exists():
        _die(f"warehouse '{db}' does not exist")

    where = [
        "b.split_factor IS NOT NULL",
        "b.split_factor > 0",
        f"(b.split_factor <= {min_ratio!r} OR b.split_factor >= {max_ratio!r})",
    ]
    if date_from:
        where.append(f"b.trade_date >= DATE '{date_from}'")
    if date_to:
        where.append(f"b.trade_date <= DATE '{date_to}'")

    join = ""
    if symbols:
        join = "JOIN _uni u ON u.symbol = b.symbol"

    sql = f"""
        SELECT b.symbol, b.trade_date, b.close, b.split_factor,
               lag(b.close) OVER (PARTITION BY b.symbol ORDER BY b.trade_date) AS prev_close
        FROM {_TABLE} b
        {join}
        QUALIFY {' AND '.join(where)}
        ORDER BY b.symbol, b.trade_date
    """
    con = duckdb.connect(str(db), read_only=True)
    try:
        if symbols:
            con.execute("CREATE TEMP TABLE _uni(symbol VARCHAR)")
            con.executemany("INSERT INTO _uni VALUES (?)", [(s,) for s in symbols])
        rows = con.execute(sql).fetchall()
    finally:
        con.close()

    out: list[dict[str, object]] = []
    for symbol, trade_date, close, factor, prev_close in rows:
        implied = (prev_close / close) if (prev_close and close) else None
        out.append(
            {
                "symbol": symbol,
                "ex_date": trade_date.isoformat()
                if isinstance(trade_date, (dt.date, dt.datetime))
                else str(trade_date),
                "price_factor": float(factor),
                "close": float(close) if close is not None else None,
                "prev_close": float(prev_close) if prev_close is not None else None,
                "implied_ratio": implied,
            }
        )
    return out


def corroborate(
    events: list[dict[str, object]], lo: float, hi: float
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    """Split declared events into (accepted, rejected) on the close-step check.

    `residual = implied_ratio * price_factor` is ~1.0 when the declared factor
    explains the observed step. An event with no prior close is ACCEPTED and
    tagged `uncorroborated` -- it cannot be checked, and refusing a real event
    is the worse error. See the module docstring for the band's justification.
    """
    accepted: list[dict[str, object]] = []
    rejected: list[dict[str, object]] = []
    for e in events:
        implied = e["implied_ratio"]
        if implied is None:
            e["residual"] = None
            e["uncorroborated"] = True
            accepted.append(e)
            continue
        residual = float(implied) * float(e["price_factor"])  # type: ignore[arg-type]
        e["residual"] = residual
        e["uncorroborated"] = False
        (accepted if lo <= residual <= hi else rejected).append(e)
    return accepted, rejected


def _fmt(v: object) -> str:
    if v is None:
        return ""
    if isinstance(v, float):
        return repr(v)
    return str(v)


def render(
    events: list[dict[str, object]],
    meta: dict[str, str],
    rejected: list[dict[str, object]] | None = None,
) -> str:
    """The exact bytes of the reference file. Deterministic: no timestamps."""
    lines = [
        "# vrp_panel --splits reference: split/reverse-split price factors.",
        "# price_factor multiplies every session STRICTLY BEFORE ex_date",
        "# (0.1 for a 10:1 forward split). Columns after price_factor are",
        "# provenance and are IGNORED by load_vrp_split_factors.",
    ]
    for key in sorted(meta):
        lines.append(f"# {key}={meta[key]}")
    for e in rejected or []:
        lines.append(
            f"# REJECTED {e['symbol']} {e['ex_date']} price_factor={_fmt(e['price_factor'])}"
            f" residual={_fmt(e['residual'])} — declared factor is not corroborated by the"
            f" close step (announcement-date artifact); excluded from the data rows."
        )
    lines.append(
        "symbol\tex_date\tprice_factor\tclose\tprev_close\timplied_ratio\tresidual\tuncorroborated"
    )
    for e in events:
        lines.append(
            "\t".join(
                (
                    str(e["symbol"]),
                    str(e["ex_date"]),
                    _fmt(e["price_factor"]),
                    _fmt(e["close"]),
                    _fmt(e["prev_close"]),
                    _fmt(e["implied_ratio"]),
                    _fmt(e.get("residual")),
                    "1" if e.get("uncorroborated") else "0",
                )
            )
        )
    return "\n".join(lines) + "\n"


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", type=Path, required=True, help="atx-db warehouse .duckdb (opened read-only)")
    p.add_argument("--out", type=Path, required=True, help="output reference TSV")
    p.add_argument("--universe", type=Path, help="symbol list, or CSV/TSV with a 'symbol' column")
    p.add_argument("--uid", action="append", default=[], help="extra symbol (repeatable)")
    p.add_argument("--from", dest="date_from", help="earliest ex_date (YYYY-MM-DD)")
    p.add_argument("--to", dest="date_to", help="latest ex_date (YYYY-MM-DD)")
    p.add_argument("--min-ratio", type=float, default=DEFAULT_MIN_RATIO)
    p.add_argument("--max-ratio", type=float, default=DEFAULT_MAX_RATIO)
    p.add_argument("--corroborate-lo", type=float, default=DEFAULT_CORROBORATE_LO)
    p.add_argument("--corroborate-hi", type=float, default=DEFAULT_CORROBORATE_HI)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not (0.0 < args.min_ratio < args.max_ratio):
        _die("require 0 < --min-ratio < --max-ratio")
    if not (0.0 < args.corroborate_lo < args.corroborate_hi):
        _die("require 0 < --corroborate-lo < --corroborate-hi")

    symbols: list[str] = []
    if args.universe:
        if not args.universe.exists():
            _die(f"--universe '{args.universe}' does not exist")
        symbols.extend(read_universe(args.universe))
    symbols.extend(s.strip() for s in args.uid if s.strip())
    symbols = sorted(set(symbols))

    declared = query_events(
        args.db, symbols, args.date_from, args.date_to, args.min_ratio, args.max_ratio
    )
    events, rejected = corroborate(declared, args.corroborate_lo, args.corroborate_hi)
    n_uncorr = sum(1 for e in events if e.get("uncorroborated"))

    meta = {
        "band": f"price_factor <= {args.min_ratio} or >= {args.max_ratio}",
        "band_source": BAND_SOURCE,
        "corroboration": f"{args.corroborate_lo:.6g} <= (prev_close/close)*price_factor"
        f" <= {args.corroborate_hi:.6g}",
        "n_declared": str(len(declared)),
        "n_events": str(len(events)),
        "n_rejected": str(len(rejected)),
        "n_symbols_scanned": str(len(symbols)) if symbols else "all",
        "n_uncorroborated": str(n_uncorr),
        "source_table": f"atx-db {_TABLE}.split_factor (vendor factor, copied unchanged)",
        "window": f"{args.date_from or 'min'}..{args.date_to or 'max'}",
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(render(events, meta, rejected), encoding="utf-8", newline="\n")

    for e in rejected:
        print(
            f"vrp_split_factors: REJECTED {e['symbol']} {e['ex_date']}"
            f" price_factor={e['price_factor']} residual={e['residual']:.4f}"
            " (not corroborated by the close step)",
            file=sys.stderr,
        )
    print(
        f"vrp_split_factors: wrote {len(events)} event(s)"
        f" ({len(rejected)} rejected, {n_uncorr} uncorroborated) -> {args.out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
