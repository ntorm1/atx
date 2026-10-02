"""Options adapter: 25-delta put/call IV at 30 days (skew), term slope, option volume and open interest by call/put,
per (security_id, session) (S8.2, the documented license need; ``docs/LICENSED_ADAPTERS.md`` section Options).

Why a license: atx-vol was removed from the tree on 2026-09-19 (commit e4bdcf54) and kept no per-security surface
history; its OPRA hive (``C:/atx-data``) is empty, and ``C:/atx-scratch`` holds three 2026-07 sessions only. The
vendor file the lake reads (SpiderRock TickerHistory3) carries earnings-censored ATM IV at 5-504 trading-day tenors,
implied/historical earnings moves and ``expiryCount`` - no delta-bucketed IV, no volume, no open interest (vendor
dictionary). The same vendor's history products do carry them, keyed by the same ``securityID``:

* ``OptionEODFeaturesHist`` (US from 2014-01-02; one row per underlier and day): ``callVolume``, ``putVolume``,
  ``callOI``, ``putOI`` (OI one day delayed), ``atmI_21d`` / ``atmI_252d``, ``delta20Skew21D``, ``vSlope_21d``;
* ``SurfaceFixedGridHist`` (US from 2010-01-04; per underlier, day and fixed term 5-504 trading days): IV at nine
  call deltas 10-90 (``volD40`` .. ``volU40``) and ``volATM``.

Raw layout expected here: ``OptionEODFeaturesHist*.parquet`` (primary) and ``SurfaceFixedGridHist*.parquet`` with a
term column (``days``). Column spellings follow the public dictionaries and are to be confirmed at purchase. Derived:
``iv_call_25d_30d`` = mean(20-delta, 30-delta call IV) and ``iv_put_25d_30d`` = mean(70-delta, 80-delta call IV)
(the 25-delta put, carry ignored) on the 21-trading-day term (= 30 calendar days); ``skew_25d_30d`` = put - call;
``term_slope_1y_30d`` = ATM(252) - ATM(21).

PIT rule ``options-pit-v1``: ``vendor_snapshot_at`` = ``srCloseTime`` (the surface snap ~5 minutes before the close)
else trading date 15:55 America/New_York; ``available_at`` = trading date 22:00 America/Chicago, the vendor's documented
US delivery (T+0), so a session's values are usable from the second following session under the 22:00 UTC cutoff.
"""

from __future__ import annotations

import datetime as dt
import math
from pathlib import Path
from typing import ClassVar

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

from .contract import Adapter, Substitute, TableSpec, ValidationReport, aliased_select, write_receipt
from .mock import UNKNOWN_TICKER, MockUniverse

DELIVERY_ZONE = "America/Chicago"
DELIVERY_TIME = "22:00:00"
TERM_30D = 21
TERM_1Y = 252
FEATURE_COLUMNS = ("vendor_security_id", "ticker", "trading_date", "call_volume", "put_volume", "call_oi", "put_oi",
                   "atm_21d", "atm_252d", "delta20_skew_21d", "sr_close_time")
FEATURE_ALIASES = {"securityid": "vendor_security_id", "ticker_tk": "ticker", "ticker": "ticker",
                   "tradingdate": "trading_date", "callvolume": "call_volume", "putvolume": "put_volume",
                   "calloi": "call_oi", "putoi": "put_oi", "atmi_21d": "atm_21d", "atmi_252d": "atm_252d",
                   "delta20skew21d": "delta20_skew_21d", "srclosetime": "sr_close_time"}
GRID_COLUMNS = ("vendor_security_id", "ticker", "trading_date", "days", "vol_atm", "vol_d40", "vol_d30", "vol_d20",
                "vol_d10", "vol_u10", "vol_u20", "vol_u30", "vol_u40", "sr_close_time")
GRID_ALIASES = {"securityid": "vendor_security_id", "ticker_tk": "ticker", "tradingdate": "trading_date",
                "term": "days", "dte": "days", "fixedterm": "days", "volatm": "vol_atm", "vold40": "vol_d40",
                "vold30": "vol_d30", "vold20": "vol_d20", "vold10": "vol_d10", "volu10": "vol_u10",
                "volu20": "vol_u20", "volu30": "vol_u30", "volu40": "vol_u40", "srclosetime": "sr_close_time"}
IV_OK = "BETWEEN 0.02 AND 5.0"

OPTIONS = TableSpec(
    name="options_daily",
    payload=(pa.field("session_date", pa.date32()), pa.field("iv_atm_30d", pa.float64()),
             pa.field("iv_call_25d_30d", pa.float64()), pa.field("iv_put_25d_30d", pa.float64()),
             pa.field("skew_25d_30d", pa.float64()), pa.field("iv_atm_1y", pa.float64()),
             pa.field("term_slope_1y_30d", pa.float64()), pa.field("call_volume", pa.float64()),
             pa.field("put_volume", pa.float64()), pa.field("call_oi", pa.float64()), pa.field("put_oi", pa.float64()),
             pa.field("delta20_skew_21d", pa.float64())),
    key=("vendor_security_id", "ticker", "session_date"),
    series=("vendor_security_id", "ticker"),
    raw_glob="OptionEODFeaturesHist*.parquet",
    raw_columns=FEATURE_COLUMNS,
    aliases=FEATURE_ALIASES, stale_days=5,
    pit_rule="trading date D visible at D 22:00 America/Chicago (vendor US delivery T+0); snapshot = srCloseTime",
)


class OptionsAdapter(Adapter):
    name = "options"
    product = "SpiderRock OptionEODFeaturesHist + SurfaceFixedGridHist (same securityID as TickerHistory3)"
    pit_rule = "options-pit-v1: D -> D 22:00 America/Chicago (documented T+0 delivery)"
    purchase = ("D3 decision point 3: SpiderRock OptionEODFeaturesHist (call/put volume and OI, ATM, 20-delta "
                "skew) + SurfaceFixedGridHist (delta grid for 25-delta at 30 d), US equities, 2019-01 onward (D7; "
                "2014 onward if the 5-year volume/OI lookbacks are wanted), daily. Same vendor and securityID as "
                "TickerHistory3, so identity is native. Alternatives: OptionMetrics IvyDB US (Volatility Surface "
                "+ Option Volume), ORATS Core history.")
    tables: ClassVar[dict[str, TableSpec]] = {"options_daily": OPTIONS}

    def _normalize(self, con, spec, files, identity, build_time):  # type: ignore[override]
        grid = sorted(files[0].parent.glob("SurfaceFixedGridHist*.parquet"))
        if grid:
            con.execute("CREATE OR REPLACE TEMP VIEW raw_grid_all AS "
                        + aliased_select(con, grid, GRID_COLUMNS, GRID_ALIASES))
        else:
            cols = ", ".join(f"CAST(NULL AS VARCHAR) AS {c}" for c in GRID_COLUMNS)
            con.execute(f"CREATE OR REPLACE TEMP VIEW raw_grid_all AS SELECT {cols}, '' AS _file LIMIT 0")
        return super()._normalize(con, spec, files, identity, build_time)

    def table_sql(self, table: str, raw: str) -> str:
        def iv(x: str) -> str:
            return f"CASE WHEN lic_num({x}) {IV_OK} THEN lic_num({x}) END"
        return f"""
            WITH g AS (
                SELECT coalesce(lic_str(vendor_security_id), lic_str(ticker)) AS gk, lic_date(trading_date) AS gd,
                       max(CASE WHEN lic_int(days) = {TERM_30D} THEN {iv('vol_atm')} END) AS atm30,
                       max(CASE WHEN lic_int(days) = {TERM_30D} THEN ({iv('vol_d30')} + {iv('vol_d20')}) / 2 END) AS c25,
                       max(CASE WHEN lic_int(days) = {TERM_30D} THEN ({iv('vol_u20')} + {iv('vol_u30')}) / 2 END) AS p25,
                       max(CASE WHEN lic_int(days) = {TERM_1Y} THEN {iv('vol_atm')} END) AS atm1y,
                       max(lic_ts(sr_close_time)) AS gclose
                FROM raw_grid_all GROUP BY 1, 2),
            f AS (SELECT *, lic_date(trading_date) AS _d, coalesce(lic_str(vendor_security_id), lic_str(ticker)) AS _k
                  FROM {raw})
            SELECT f._file, CAST(NULL AS VARCHAR) AS cusip, lic_str(f.ticker) AS ticker,
                   lic_str(f.vendor_security_id) AS vendor_security_id,
                   lic_int(f.vendor_security_id) AS native_security_id, f._d AS id_date, f._d AS session_date,
                   coalesce(g.atm30, {iv('f.atm_21d')}) AS iv_atm_30d, g.c25 AS iv_call_25d_30d, g.p25 AS iv_put_25d_30d,
                   g.p25 - g.c25 AS skew_25d_30d, coalesce(g.atm1y, {iv('f.atm_252d')}) AS iv_atm_1y,
                   coalesce(g.atm1y, {iv('f.atm_252d')}) - coalesce(g.atm30, {iv('f.atm_21d')}) AS term_slope_1y_30d,
                   lic_num(f.call_volume) AS call_volume, lic_num(f.put_volume) AS put_volume,
                   lic_num(f.call_oi) AS call_oi, lic_num(f.put_oi) AS put_oi,
                   lic_num(f.delta20_skew_21d) AS delta20_skew_21d,
                   coalesce(lic_ts(f.sr_close_time), g.gclose, lic_utc(f._d, TIME '15:55:00', 'America/New_York'))
                       AS vendor_snapshot_at,
                   lic_utc(f._d, TIME '{DELIVERY_TIME}', '{DELIVERY_ZONE}') AS rule_available_at,
                   'publication_lag' AS rule_basis, false AS rule_vintage_risk,
                   CASE WHEN f._d IS NULL THEN 'missing_date' WHEN f._k IS NULL THEN 'missing_identifier' END AS _reject
            FROM f LEFT JOIN g ON g.gk = f._k AND g.gd = f._d"""

    def extra_checks(self, con: duckdb.DuckDBPyConnection, table: str, report: ValidationReport) -> None:
        report.add(table, "volume_oi_nonnegative", con.execute("""
            SELECT count(*) FROM t_options_daily WHERE call_volume < 0 OR put_volume < 0 OR call_oi < 0 OR put_oi < 0
            """).fetchone()[0])
        report.add(table, "iv_domain", con.execute(f"""
            SELECT count(*) FROM t_options_daily WHERE iv_atm_30d NOT {IV_OK} OR iv_call_25d_30d NOT {IV_OK}
                OR iv_put_25d_30d NOT {IV_OK}""").fetchone()[0])

    def substitute(self) -> Substitute:
        return Substitute(
            stage="options", owner="LIC (S8.2, alpha_panel/options.py)", keys=("session_date", "security_id"),
            columns={"iv_atm_30d": "iv_atm_21d", "iv_atm_1y": "iv_atm_252d", "term_slope_1y_30d": "term_slope_252_21"},
            note=("Free part: TickerHistory3 earnings-censored ATM IV term structure (30 d ~ the 21-trading-day tenor) "
                  "and its term slopes. No free skew, option volume or open interest."))

    def mock(self, raw_dir: Path, universe: MockUniverse | None = None, seed: int = 0) -> list[Path]:
        u = universe or MockUniverse()
        rng = u.rng
        raw_dir = Path(raw_dir)
        raw_dir.mkdir(parents=True, exist_ok=True)
        feats: dict[str, list] = {c: [] for c in ("securityID", "ticker_tk", "tradingDate", "callVolume", "putVolume",
                                                  "callOI", "putOI", "atmI_21d", "atmI_252d", "delta20Skew21D",
                                                  "srCloseTime")}
        grid: dict[str, list] = {c: [] for c in ("securityID", "ticker_tk", "tradingDate", "days", "volATM", "volD40",
                                                 "volD30", "volD20", "volD10", "volU10", "volU20", "volU30", "volU40",
                                                 "srCloseTime")}
        for d in u.weekdays(dt.date(2024, 6, 3), dt.date(2024, 7, 31)):
            close = dt.datetime.combine(d, dt.time(19, 55))  # 15:55 EDT
            for i, ln in enumerate(u.lines):
                if not ln.alive(d):
                    continue
                sid = None if i == 9 else ln.security_id  # line 9: vendor row without securityID (ticker only)
                atm = 0.2 + 0.03 * (i % 7) + rng.uniform(-0.01, 0.01)
                cv, pv = float(rng.randrange(100, 50000)), float(rng.randrange(100, 40000))
                feats["securityID"].append(sid)
                feats["ticker_tk"].append(ln.ticker_on(d))
                feats["tradingDate"].append(d)
                feats["callVolume"].append(cv)
                feats["putVolume"].append(pv)
                feats["callOI"].append(cv * 20)
                feats["putOI"].append(pv * 25)
                feats["atmI_21d"].append(atm)
                feats["atmI_252d"].append(atm + 0.02)
                feats["delta20Skew21D"].append(0.05)
                feats["srCloseTime"].append(close)
                for days in (5, 21, 63, 252):
                    a = atm + 0.02 * math.log(days / 21 + 1e-9) / math.log(12)
                    grid["securityID"].append(sid)
                    grid["ticker_tk"].append(ln.ticker_on(d))
                    grid["tradingDate"].append(d)
                    grid["days"].append(days)
                    grid["volATM"].append(a)
                    for k, name in enumerate(("volD40", "volD30", "volD20", "volD10", "volU10", "volU20", "volU30",
                                              "volU40")):
                        call_delta = 0.1 * (k + 1 + (k >= 4))  # 0.1..0.4, 0.6..0.9
                        grid[name].append(a * (1 + 0.25 * (call_delta - 0.5) ** 2 + 0.15 * (call_delta - 0.5)))
                    grid["srCloseTime"].append(close)
            feats["securityID"].append(None)
            feats["ticker_tk"].append(UNKNOWN_TICKER)
            feats["tradingDate"].append(d)
            for c in ("callVolume", "putVolume", "callOI", "putOI", "atmI_21d", "atmI_252d", "delta20Skew21D"):
                feats[c].append(1.0 if "Volume" in c or "OI" in c else 0.3)
            feats["srCloseTime"].append(close)
        fp = raw_dir / "OptionEODFeaturesHist_2024.parquet"
        gp = raw_dir / "SurfaceFixedGridHist_2024.parquet"
        pq.write_table(pa.table(feats), fp)
        pq.write_table(pa.table(grid), gp)
        for p in (fp, gp):
            write_receipt(raw_dir, p, fetched_at=dt.datetime(2026, 7, 1, 6, 0), history_mode="pit_archive",
                          url=f"mock://spiderrock/{p.name}", http_status=200)
        return [fp, gp]


ADAPTER = OptionsAdapter()
