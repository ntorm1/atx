"""End-to-end test for ``atxvol.build_surface_db`` (the one-call production build
driver binding) plus a reopen through the ``SurfaceDb`` binding.

The fixture writes a tiny date-partitioned OPRA hive v2 tree with pyarrow,
mirroring the C++ Task-2 synthetic generator (``tests/support/synthetic_opra_hive.hpp``):
2 symbols x 2 dates, 9 strikes 80..120, +28/+56d expiries, put-call-parity C/P
pairs so the loader can imply the spot. Prices are Black-European mids under a
quadratic vol smile, computed at r = 0 because the build driver has no rate knob
(``OpraHiveSpec.r`` defaults to 0.0) — a nonzero-r fixture would break the fits.
"""

from __future__ import annotations

import datetime as dt
import math

import pyarrow as pa
import pyarrow.parquet as pq

import atxvol

# Exact 8-column schema the C++ loaders consume (design §3; matches the pull tool
# and tests/support/synthetic_opra_hive.hpp).
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
    """Flat-rate Black (q=0) European price — plants PCP-consistent mids."""
    v = sigma * math.sqrt(t)
    d1 = (math.log(s / k) + (r + 0.5 * sigma * sigma) * t) / v
    d2 = d1 - v
    disc = math.exp(-r * t)
    if is_call:
        return s * _norm_cdf(d1) - k * disc * _norm_cdf(d2)
    return k * disc * _norm_cdf(-d2) - s * _norm_cdf(-d1)


def _osi(root: str, expiry: dt.date, cp: str, strike: float) -> str:
    """OSI/OCC 21-char symbol: 6-char space-padded root + YYMMDD + {C|P} + strike*1000."""
    return f"{root:<6}{expiry:%y%m%d}{cp}{int(round(strike * 1000.0)):08d}"


def _write_hive(root, symbols, dates) -> None:
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
        # Sort by (underlying, symbol) — the on-disk order the loader expects.
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
        date_dir = root / f"date={date}"
        date_dir.mkdir(parents=True, exist_ok=True)
        pq.write_table(table, date_dir / "data.parquet")


def test_build_surface_db_populates_and_reopens(tmp_path) -> None:
    hive_root = tmp_path / "hive"
    db_root = tmp_path / "db"
    symbols = ["AAA", "BBB"]
    dates = ["2026-07-01", "2026-07-02"]
    _write_hive(hive_root, symbols, dates)

    report = atxvol.build_surface_db(
        db_root=str(db_root),
        hive_root=str(hive_root),
        date_lo="2026-07-01",
        date_hi="2026-07-02",
    )

    # Report is a plain nested dict mirroring SurfaceDbBuildReport.
    assert isinstance(report, dict)
    cov = report["coverage"]
    assert cov["cells_loaded"] == 4  # 2 symbols x 2 dates
    assert cov["cells_to_fit"] == 4  # all new this run
    assert cov["cells_ok"] == 4  # every board fit
    assert cov["cells_failed"] == 0
    assert cov["dates_total"] == 2
    assert cov["dates_written"] == 2
    assert report["config"]["n_symbols"] == 2
    assert report["config"]["n_configured"] == 2
    assert report["config"]["n_disabled_failed"] == 0
    assert report["n_dates_loaded"] == 2
    assert report["n_dates_missing"] == 0
    assert report["n_load_errors"] == 0

    # Reopen the built db through the SurfaceDb binding.
    sdb = atxvol.SurfaceDb.open(str(db_root))
    keys = sorted(sdb.partitions())
    assert keys == dates  # one partition per date
    assert set(sdb.symbols()) == set(symbols)

    # Both the owned and zero-copy load paths succeed for a (date, symbol) cell.
    priced = sdb.load_surface(keys[0], "AAA")
    assert priced is not None
    loaded = sdb.map_surface(keys[0], "AAA")
    assert loaded is not None
    # ATM within the fit's expiry span -> a finite implied vol.
    assert math.isfinite(loaded.iv(100.0, 0.10))
    assert math.isfinite(priced.iv(100.0, 0.10))

    # Second build over the unchanged hive re-fits nothing (cell-aware resume).
    report2 = atxvol.build_surface_db(
        db_root=str(db_root),
        hive_root=str(hive_root),
        date_lo="2026-07-01",
        date_hi="2026-07-02",
    )
    assert report2["coverage"]["cells_to_fit"] == 0
    assert report2["coverage"]["cells_already_present"] == 4
    assert report2["coverage"]["dates_written"] == 0
    assert report2["config"]["n_skipped_existing"] == 2
