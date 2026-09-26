"""VA1: the shared repaired adjusted close neutralizes a vendor factor-decrease artifact (small and
large, raw price flat), keeps a genuine dividend and a genuine reverse split (raw price followed the
factor), never repairs across a data gap, and gives the same returns on a date-scoped read."""

from __future__ import annotations

import datetime as dt

import duckdb
import pytest

from atx_db._forward_return_publication import selected_bars_sql

D = [dt.date(2020, 12, 29), dt.date(2020, 12, 30), dt.date(2020, 12, 31), dt.date(2021, 1, 4),
     dt.date(2021, 1, 5), dt.date(2021, 1, 6)]
ART = 0.97
# line -> [(date, close, vendor factor adjusted/close)]
BARS = {
    # 1% dividend ex 12-31 (factor up, total return 0), then the 2021-01-04 artifact (factor x0.97, price flat)
    "DIV": list(zip(D, [100, 100, 99, 99, 99.5, 99.5], [1, 1, 100 / 99, 100 / 99 * ART, 100 / 99 * ART,
                                                         100 / 99 * ART], strict=True)),
    # genuine reverse split with an inexact vendor ratio on 2021-01-04: raw close x7.29, adjusted flat
    "RS": list(zip(D, [10, 10, 10, 10 / 0.1372, 10 / 0.1372, 10 / 0.1372], [1, 1, 1, 0.1372, 0.1372, 0.1372],
                   strict=True)),
    # large artifact (inexact factor x0.0413) with the raw close up 1%
    "BIG": list(zip(D, [50, 50, 50, 50.5, 50.5, 51], [1, 1, 1, 0.0413, 0.0413, 0.0413], strict=True)),
    # in-band inexact artifact (x0.93) on a day the raw close rose 5%: in R1d's non-split band, repaired
    "INB": list(zip(D, [40, 40, 40, 42, 42, 42], [1, 1, 1, 0.93, 0.93, 0.93], strict=True)),
    # an exact-ratio decrease (x0.5) with a flat price is an R1d split hazard: never repaired here
    "EXACT": list(zip(D, [30, 30, 30, 30, 30, 30], [1, 1, 1, 0.5, 0.5, 0.5], strict=True)),
    # a factor decrease across a 21-day data gap is never repaired (P8: factor_step_across_data_gap)
    "GAP": [(dt.date(2020, 12, 14), 20, 1), (dt.date(2021, 1, 4), 20, ART), (dt.date(2021, 1, 5), 20.2, ART)],
}


def _returns(con, basis, security_filter="TRUE"):
    rows = con.execute(f"""
        SELECT security_id, trade_date, price / lag(price) OVER (PARTITION BY security_id ORDER BY trade_date) - 1
        FROM ({selected_bars_sql(basis, security_filter=security_filter)}) b""", [None, None]).fetchall()
    return {(s, d): r for s, d, r in rows if r is not None}


def test_artifact_neutralized_dividend_and_reverse_split_kept():
    con = duckdb.connect(config={"threads": 1, "memory_limit": "256MB"})
    con.execute("""CREATE TABLE equity_daily_bars (source VARCHAR, security_id VARCHAR, symbol VARCHAR,
        vendor_security_id VARCHAR, trade_date DATE, close DOUBLE, adjusted_close DOUBLE, available_at TIMESTAMP,
        source_loaded_at TIMESTAMP)""")
    con.executemany("INSERT INTO equity_daily_bars VALUES ('v', ?, ?, 'x', ?, ?, ?, ?, '2026-01-01')",
                    [(line, line, day, close, close * factor, dt.datetime.combine(day, dt.time(22)))
                     for line, bars in BARS.items() for day, close, factor in bars])
    art = D[3]
    repaired, raw = _returns(con, "adjusted_close"), _returns(con, "close")
    assert repaired[("DIV", D[2])] == pytest.approx(0.0, abs=1e-12)              # dividend kept: total return 0
    assert raw[("DIV", D[2])] == pytest.approx(-0.01)
    assert repaired[("DIV", art)] == pytest.approx(0.0, abs=1e-12)               # vendor: -3.0%
    assert repaired[("BIG", art)] == pytest.approx(0.01, abs=1e-12)              # vendor: -95.8%
    assert repaired[("INB", art)] == pytest.approx(0.05, abs=1e-12)              # vendor: -2.35%
    assert repaired[("RS", art)] == pytest.approx(0.0, abs=1e-12)                # reverse split kept
    assert repaired[("EXACT", art)] == pytest.approx(-0.5, abs=1e-12)            # R1d hazard: untouched
    assert repaired[("GAP", art)] == pytest.approx(ART - 1, abs=1e-12)           # across a gap: untouched
    for key, value in raw.items():                                                # every other day: raw x factor
        if key[1] not in (D[2], art):
            assert repaired[key] == pytest.approx(value, abs=1e-12)
    # a contiguous date-scoped read (as the event and factor-return builders do) gives the same returns
    scoped = _returns(con, "adjusted_close", "trade_date >= DATE '2020-12-30'")
    assert scoped == pytest.approx({k: v for k, v in repaired.items() if k[1] > D[1] and k != ("GAP", art)},
                                   abs=1e-12)
    # the dividend payer over the window: raw -0.5% plus the 1% dividend, the artifact gone
    first, last = (con.execute(f"""SELECT arg_min(price, trade_date), arg_max(price, trade_date)
        FROM ({selected_bars_sql('adjusted_close', security_filter="security_id = 'DIV'")}) b""",
                              [None, None]).fetchone())
    assert last / first - 1 == pytest.approx(99.5 / 99 - 1, abs=1e-12)
