#!/usr/bin/env python3
"""Offline staging for the atx regime loader.

Downloads public macro series to a staging dir as CSVs that
`atx-impl regime --staging-dir <dir>` ingests. Stdlib only; no deps.
The staged snapshot is the reproducible input (re-running the loader on the
same snapshot yields a byte-identical .seg).
"""
import sys, urllib.request, pathlib

FRED = {
    "vix":    "VIXCLS",
    "dgs2":   "DGS2",
    "dgs10":  "DGS10",
    "hy_oas": "BAMLH0A0HYM2",
    "ig_oas": "BAMLC0A0CM",
    "nfci":   "NFCI",
}

# vvix (CBOE VVIX) and move (ICE BofA MOVE) do not have a stable,
# freely accessible CSV endpoint.  Stage them manually:
#   vvix: https://www.cboe.com/tradable_products/vix/vix_historical_data/
#          → download CSV, rename to vvix.csv, place in <staging-dir>
#   move: https://finance.yahoo.com/quote/%5EMOVE/history/
#          (Yahoo Finance history download)  → rename to move.csv
# The loader (stage_regime.cpp) silently skips any file that is absent from
# the staging dir, so a FRED-only run is valid — vvix/move columns are simply
# omitted from the resulting .seg.


def fetch_fred(series_id: str) -> bytes:
    url = f"https://fred.stlouisfed.org/graph/fredgraph.csv?id={series_id}"
    with urllib.request.urlopen(url, timeout=60) as r:  # noqa: S310
        return r.read()


def main(out_dir: str) -> int:
    out = pathlib.Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for name, sid in FRED.items():
        (out / f"{name}.csv").write_bytes(fetch_fred(sid))
        print(f"staged {name} <- FRED {sid}")
    print(
        "NOTE: vvix (CBOE) and move (Yahoo/ICE) must be staged manually; "
        "see atx-engine/include/atx/engine/regime/README.md for source URLs."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else "data/regime_staging"))
