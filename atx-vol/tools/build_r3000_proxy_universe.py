#!/usr/bin/env python3
"""Build a Russell-3000 proxy universe: top-N US common stocks by average dollar volume.

Source: the local Databento EQUS.SUMMARY OHLCV-1d parquet hive
(data/databento/equs_ohlcv_1d_by_date). Ranking window: the last 21 sessions at or
before --as-of. ETFs/funds are excluded by issuer-name pattern (joined from the
atx-impl security master on symbol); unit/warrant/right share classes are excluded
by symbol pattern. This is a size proxy (dollar volume), not official FTSE Russell
membership -- adequate for whole-universe fitting stress tests where the goal is
3000 large, diverse, mostly-optionable US single names.

Output: a symbols text file (one per line, ranked) plus a metadata CSV.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

import duckdb

ETF_NAME_PATTERNS = [
    " ETF", "ISHARES", "SPDR", "INVESCO", "VANGUARD", "PROSHARES", "DIREXION",
    "WISDOMTREE", "FIRST TRUST", "GLOBAL X", "GRANITESHARES", "VANECK",
    "TRUST, SERIES", "INDEX FUND", "BOND FUND", "INCOME FUND", " FUND",
    "SELECT SECTOR", "ULTRASHORT", "ULTRAPRO", "2X ", "3X ", "-1X ",
]

# Units / warrants / rights / when-issued style suffixes (uppercase symbols).
BAD_SYMBOL_RE = re.compile(r"^[A-Z]{4}[UWR]$|\.(?:U|W|WS|R|RT)$")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--equs-root", type=pathlib.Path,
                        default=pathlib.Path("data/databento/equs_ohlcv_1d_by_date"))
    parser.add_argument("--master-db", type=pathlib.Path,
                        default=pathlib.Path("atx-impl/db/atx_impl.duckdb"))
    parser.add_argument("--as-of", default="2026-06-05")
    parser.add_argument("--sessions", type=int, default=21)
    parser.add_argument("--top", type=int, default=3000)
    parser.add_argument("--min-price", type=float, default=1.0)
    parser.add_argument("--out-symbols", type=pathlib.Path,
                        default=pathlib.Path("data/universe/r3000_proxy_symbols.txt"))
    parser.add_argument("--out-meta", type=pathlib.Path,
                        default=pathlib.Path("data/universe/r3000_proxy_meta.csv"))
    args = parser.parse_args()

    con = duckdb.connect()
    glob = str(args.equs_root / "date=*" / "*.parquet").replace("\\", "/")

    dates = [str(r[0]) for r in con.execute(
        f"""
        SELECT DISTINCT date AS d
        FROM read_parquet('{glob}')
        WHERE d <= DATE '{args.as_of}' ORDER BY d DESC LIMIT {args.sessions}
        """).fetchall()]
    if len(dates) < args.sessions:
        print(f"warning: only {len(dates)} sessions available <= {args.as_of}", file=sys.stderr)
    lo, hi = min(dates), max(dates)
    print(f"ranking window: {lo}..{hi} ({len(dates)} sessions)")

    ranked = con.execute(
        f"""
        SELECT symbol,
               avg(close * volume) AS adv_usd,
               avg(close)          AS avg_px,
               count(*)            AS n_days
        FROM read_parquet('{glob}')
        WHERE date BETWEEN DATE '{lo}' AND DATE '{hi}'
        GROUP BY symbol
        HAVING n_days >= {max(1, len(dates) * 3 // 4)} AND avg_px >= {args.min_price}
        ORDER BY adv_usd DESC
        """).fetchdf()
    print(f"candidates after liquidity/price gates: {len(ranked)}")

    names: dict[str, str] = {}
    if args.master_db.exists():
        mcon = duckdb.connect(str(args.master_db), read_only=True)
        for sym, name in mcon.execute(
                "SELECT primary_symbol, max(name) FROM v_security_master_current "
                "GROUP BY primary_symbol").fetchall():
            names[sym] = (name or "").upper()
        mcon.close()

    def is_fund(sym: str) -> bool:
        # master stores class shares without the dot (BRK.B -> BRKB)
        name = names.get(sym) or names.get(sym.replace(".", ""), "")
        return any(pat in name for pat in ETF_NAME_PATTERNS)

    selected: list[tuple[str, float, str]] = []
    n_fund, n_badsym = 0, 0
    for row in ranked.itertuples(index=False):
        sym = str(row.symbol)
        if BAD_SYMBOL_RE.search(sym):
            n_badsym += 1
            continue
        if is_fund(sym):
            n_fund += 1
            continue
        selected.append((sym, float(row.adv_usd), names.get(sym, names.get(sym.replace(".", ""), ""))))
        if len(selected) == args.top:
            break

    print(f"excluded: {n_fund} fund-name matches, {n_badsym} unit/warrant symbols")
    print(f"selected: {len(selected)}")

    args.out_symbols.parent.mkdir(parents=True, exist_ok=True)
    args.out_symbols.write_text("\n".join(s for s, _, _ in selected) + "\n", encoding="ascii")
    with args.out_meta.open("w", encoding="utf-8", newline="") as fh:
        fh.write("rank,symbol,adv_usd,name\n")
        for i, (sym, adv, name) in enumerate(selected, 1):
            fh.write(f'{i},{sym},{adv:.0f},"{name}"\n')
    print(f"wrote {args.out_symbols} and {args.out_meta}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
