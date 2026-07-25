"""Standalone OPRA hive parquet fixture writer for ``test_surface_db_build.py``.

Deliberately a real, importable/lintable script rather than an inline string,
run as a subprocess with its own fresh interpreter that never imports
``atxvol``. See the DLL-collision note in ``atx-vol/python/README.md`` (and
the module docstring in ``test_surface_db_build.py``): ``atxvol._core`` and
``pyarrow`` ship incompatible same-named Arrow DLLs and cannot both be
imported in one process on Windows, in either import order.

Mirrors the C++ Task-2 synthetic generator
(``tests/support/synthetic_opra_hive.hpp``): N symbols x N dates, 9 strikes
80..120, +28/+56d expiries, put-call-parity C/P pairs so the loader can imply
the spot. Prices are Black-European mids under a quadratic vol smile, computed
at r = 0 because the build driver has no rate knob (``OpraHiveSpec.r``
defaults to 0.0) -- a nonzero-r fixture would break the fits.

Invocation: ``python _gen_opra_hive.py '<json payload>'`` where payload is
``{"root": str, "symbols": [str, ...], "dates": ["YYYY-MM-DD", ...]}``.
"""

from __future__ import annotations

import datetime as dt
import json
import math
import pathlib
import sys

import pyarrow as pa
import pyarrow.parquet as pq

# Exact 8-column schema the C++ loaders consume (design Sec.3; matches the pull
# tool and tests/support/synthetic_opra_hive.hpp).
_SCHEMA = pa.schema(
    [
        ("ts", pa.timestamp("ns")),
        ("underlying", pa.string()),
        ("symbol", pa.string()),
        ("instrument_id", pa.int64()),
        ("bid_px", pa.int64()),
        ("ask_px", pa.int64()),
        ("bid_sz", pa.int64()),
        ("ask_sz", pa.int64()),
    ]
)

_STRIKES = [80.0, 85.0, 90.0, 95.0, 100.0, 105.0, 110.0, 115.0, 120.0]
_DTES = [28, 56]
_SPOT = 100.0
_R = 0.0
_SECONDS_PER_YEAR = 365.0 * 24.0 * 3600.0
_SNAP_SECONDS = 19 * 3600 + 55 * 60  # 19:55:00 pre-close snapshot


def _norm_cdf(x: float) -> float:
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def _black_price(s: float, k: float, t: float, r: float, sigma: float, is_call: bool) -> float:
    # Flat-rate Black (q=0) European price -- plants PCP-consistent mids.
    v = sigma * math.sqrt(t)
    d1 = (math.log(s / k) + (r + 0.5 * sigma * sigma) * t) / v
    d2 = d1 - v
    disc = math.exp(-r * t)
    if is_call:
        return s * _norm_cdf(d1) - k * disc * _norm_cdf(d2)
    return k * disc * _norm_cdf(-d2) - s * _norm_cdf(-d1)


def _osi(root: str, expiry: dt.date, cp: str, strike: float) -> str:
    # OSI/OCC 21-char symbol: 6-char space-padded root + YYMMDD + {C|P} + strike*1000.
    return f"{root:<6}{expiry:%y%m%d}{cp}{int(round(strike * 1000.0)):08d}"


def write_hive(root: str, symbols: list[str], dates: list[str]) -> None:
    """Write ``<root>/date=<d>/data.parquet`` for every date, all symbols per file."""
    counter = 1
    for date in dates:
        y, m, d = (int(p) for p in date.split("-"))
        trade = dt.date(y, m, d)
        ts = dt.datetime(y, m, d, 19, 55, 0)  # tz-naive UTC wall clock
        rows = []
        for sym in symbols:
            for dte in _DTES:
                expiry = trade + dt.timedelta(days=dte)
                t = (dte * 86400 - _SNAP_SECONDS) / _SECONDS_PER_YEAR
                for k in _STRIKES:
                    sigma = 0.25 + 0.02 * ((k / _SPOT) - 1.0) ** 2
                    for cp in ("C", "P"):
                        mid = _black_price(_SPOT, k, t, _R, sigma, cp == "C")
                        rows.append(
                            {
                                "ts": ts,
                                "underlying": sym,
                                "symbol": _osi(sym, expiry, cp, k),
                                "bid_px": int(round(0.98 * mid * 1e9)),
                                "ask_px": int(round(1.02 * mid * 1e9)),
                            }
                        )
        # Sort by (underlying, symbol) -- the on-disk order the loader expects.
        rows.sort(key=lambda r: (r["underlying"], r["symbol"]))
        table = pa.Table.from_pydict(
            {
                "ts": [r["ts"] for r in rows],
                "underlying": [r["underlying"] for r in rows],
                "symbol": [r["symbol"] for r in rows],
                "instrument_id": list(range(counter, counter + len(rows))),
                "bid_px": [r["bid_px"] for r in rows],
                "ask_px": [r["ask_px"] for r in rows],
                "bid_sz": [10] * len(rows),
                "ask_sz": [10] * len(rows),
            },
            schema=_SCHEMA,
        )
        counter += len(rows)
        date_dir = pathlib.Path(root) / f"date={date}"
        date_dir.mkdir(parents=True, exist_ok=True)
        pq.write_table(table, date_dir / "data.parquet")


if __name__ == "__main__":
    _args = json.loads(sys.argv[1])
    write_hive(_args["root"], _args["symbols"], _args["dates"])
