"""GICS adapter: dated sector / industry group / industry / sub-industry per security (S8.1).

Raw layout (``gics_*.csv``): one row per classification spell as the vendor publishes it (S&P/MSCI GICS Direct
history, or a Compustat ``co_hgic``-style extract re-keyed to CUSIP/ticker): CUSIP, TICKER, GSUBIND (8 digits),
INDFROM, INDTHRU, SNAPSHOTDATE (the vendor record's publication date; absent in most history extracts). The
sector/group/industry codes are the 2/4/6-digit prefixes of GSUBIND.

PIT rule ``gics-pit-v1``: a spell "code X from F" is known at its publication (SNAPSHOTDATE 23:59:59 America/New_York;
reclassifications are announced before F) and applies to sessions d >= F; without a publication date it is known
at F 00:00 ET (``floor``, ``vintage_risk``). Consumers need both ``available_at < cutoff(d)`` and
``effective_from <= d < effective_to``. Vendor histories restated to today's structure (the classic GICS backfill:
Alphabet in Communication Services before that sector's Media & Entertainment group existed) are caught by the
structure guard: a code used before its structure start has ``effective_from`` clipped to that start
(``clock_basis = structure_guard``, ``vintage_risk``); a code used after its discontinuation fails
``structure_ok``. ``GICS_STRUCTURE`` is a seed of the known breaks; extend it from the vendor's structure file.

The vendor price file (TickerHistory3) has a ``GICS`` column, but it is empty in every lake row 2018-2026
(prices stage: 0 non-null of 21.9M rows), so this adapter stays mock-only until D3.
"""

from __future__ import annotations

import datetime as dt
from pathlib import Path
from typing import ClassVar

import duckdb
import pyarrow as pa

from .contract import Adapter, Substitute, TableSpec, ValidationReport, csv_write, write_receipt
from .mock import MockUniverse

GICS_STRUCTURE: tuple[tuple[str, dt.date | None, dt.date | None, str], ...] = (
    ("60", dt.date(2016, 9, 1), None, "Real Estate sector split from Financials after the 2016-08-31 close"),
    ("5020", dt.date(2018, 10, 1), None, "Media & Entertainment group (Communication Services) after 2018-09-28"),
    ("40201060", dt.date(2023, 3, 20), None, "Transaction & Payment Processing Services after the 2023-03-17 close"),
    ("45102020", None, dt.date(2023, 3, 17), "Data Processing & Outsourced Services discontinued 2023-03-17"),
)
ALIASES = {"gsubind": "gics_sub_industry", "subindustry": "gics_sub_industry", "gics_code": "gics_sub_industry",
           "gics": "gics_sub_industry", "indfrom": "effective_from", "indthru": "effective_to",
           "from_date": "effective_from", "to_date": "effective_to", "snapshotdate": "snapshot_date",
           "publication_date": "snapshot_date", "asof": "snapshot_date", "symbol": "ticker"}
GICS = TableSpec(
    name="gics_history",
    payload=(pa.field("gics_sector", pa.string()), pa.field("gics_group", pa.string()),
             pa.field("gics_industry", pa.string()), pa.field("gics_sub_industry", pa.string()),
             pa.field("effective_from", pa.date32()), pa.field("effective_to", pa.date32()),
             pa.field("effective_from_vendor", pa.date32()), pa.field("structure_ok", pa.bool_())),
    key=("cusip", "ticker", "effective_from_vendor", "vendor_snapshot_at"),
    series=("cusip", "ticker", "effective_from_vendor"),
    raw_glob="gics_*.csv",
    raw_columns=("vendor_security_id", "cusip", "ticker", "gics_sub_industry", "effective_from", "effective_to",
                 "snapshot_date"),
    aliases=ALIASES, stale_days=None,
    pit_rule="known at SNAPSHOTDATE 23:59:59 ET (else effective_from 00:00 ET, floor); applies from effective_from",
)


def _structure_values() -> str:
    def lit(d: dt.date | None) -> str:
        return f"DATE '{d}'" if d else "CAST(NULL AS DATE)"
    return ", ".join(f"('{p}', {lit(a)}, {lit(b)})" for p, a, b, _ in GICS_STRUCTURE)


class GicsAdapter(Adapter):
    name = "gics"
    product = "S&P/MSCI GICS Direct history (or Compustat co_hgic re-keyed to CUSIP)"
    pit_rule = "gics-pit-v1: publication clock + effective date; structure guard against restated histories"
    purchase = ("D3 decision point 5 (low): GICS Direct history with publication dates, 2015+; only if a risk "
                "model or client mandate needs GICS itself. SIC/FF/NAICS and text industries cover research use.")
    tables: ClassVar[dict[str, TableSpec]] = {"gics_history": GICS}

    def table_sql(self, table: str, raw: str) -> str:
        sub = "regexp_replace(lic_str(gics_sub_industry), '[^0-9]', '', 'g')"
        return f"""
            WITH r AS (SELECT *, {sub} AS _sub, lic_date(effective_from) AS _from, lic_date(effective_to) AS _to,
                              lic_date(snapshot_date) AS _snap FROM {raw}),
            st(prefix, valid_from, valid_to) AS (VALUES {_structure_values()}),
            g AS (
                SELECT r.*, max(st.valid_from) AS _start, min(st.valid_to) AS _end
                FROM r LEFT JOIN st ON starts_with(r._sub, st.prefix) GROUP BY ALL
            )
            SELECT _file, lic_str(cusip) AS cusip, lic_str(ticker) AS ticker,
                   lic_str(vendor_security_id) AS vendor_security_id, CAST(NULL AS BIGINT) AS native_security_id,
                   greatest(_from, _start) AS id_date,
                   left(_sub, 2) AS gics_sector, left(_sub, 4) AS gics_group, left(_sub, 6) AS gics_industry,
                   _sub AS gics_sub_industry, greatest(_from, _start) AS effective_from, _to AS effective_to,
                   _from AS effective_from_vendor, _end IS NULL OR greatest(_from, _start) <= _end AS structure_ok,
                   CASE WHEN _snap IS NOT NULL THEN lic_utc(_snap, TIME '23:59:59', 'America/New_York')
                        ELSE lic_utc(greatest(_from, _start), TIME '00:00:00', 'America/New_York') END AS vendor_snapshot_at,
                   CASE WHEN _snap IS NOT NULL THEN lic_utc(_snap, TIME '23:59:59', 'America/New_York')
                        ELSE lic_utc(greatest(_from, _start), TIME '00:00:00', 'America/New_York') END AS rule_available_at,
                   CASE WHEN _from < _start THEN 'structure_guard' WHEN _snap IS NULL THEN 'floor'
                        ELSE 'vendor_pit' END AS rule_basis,
                   _from < _start OR _snap IS NULL AS rule_vintage_risk,
                   CASE WHEN length(_sub) <> 8 THEN 'bad_code' WHEN _from IS NULL THEN 'missing_effective_from'
                        WHEN _to < _from THEN 'inverted_spell' END AS _reject
            FROM g"""

    def extra_checks(self, con: duckdb.DuckDBPyConnection, table: str, report: ValidationReport) -> None:
        report.add(table, "structure_ok", con.execute(
            "SELECT count(*) FROM t_gics_history WHERE NOT structure_ok").fetchone()[0], fatal=False)
        report.add(table, "spells_overlap", con.execute("""
            SELECT count(*) FROM (
                SELECT security_id, effective_from, lag(coalesce(effective_to, DATE '9999-12-31'))
                       OVER (PARTITION BY security_id ORDER BY effective_from) AS prev_to
                FROM (SELECT DISTINCT ON (security_id, effective_from) * FROM t_gics_history
                      WHERE security_id IS NOT NULL ORDER BY security_id, effective_from, vendor_snapshot_at DESC))
            WHERE effective_from <= prev_to""").fetchone()[0], fatal=False)

    def substitute(self) -> Substitute:
        return Substitute(
            stage="classification", owner="MKT (S7.1)", keys=("cik",),
            columns={"gics_sector": "ff12", "gics_industry": "ff49", "gics_sub_industry": "naics"},
            note=("SIC -> NAICS (approximate) and Fama-French 5-49 industries per issuer, dated at the filing clock; "
                  "text industries (TNIC-style, lane TXT S7.2) for peer sets."))

    def mock(self, raw_dir: Path, universe: MockUniverse | None = None, seed: int = 0) -> list[Path]:
        u = universe or MockUniverse()
        rng = u.rng
        raw_dir = Path(raw_dir)
        raw_dir.mkdir(parents=True, exist_ok=True)
        subs = ["20106010", "25502020", "35201010", "40101015", "45103010", "15101050", "10102020", "55105020"]
        rows = []
        for i, ln in enumerate(u.lines):
            if i == 0:  # 2023 reclassification announced 2023-03-06, effective 2023-03-20
                rows.append([ln.cusips[0][0], ln.tickers[0][0], "45102020", "2019-01-02", "2023-03-19", "2018-12-20"])
                rows.append([ln.cusips[0][0], ln.tickers[-1][0], "40201060", "2023-03-20", None, "2023-03-06"])
                continue
            if i == 8:  # restated history: today's code applied back to 2015 (a GICS backfill)
                rows.append([ln.cusips[0][0], ln.tickers[0][0], "50203010", "2015-01-02", None, None])
                continue
            rows.append([ln.cusip_on(ln.first), ln.ticker_on(ln.first), rng.choice(subs), ln.first.isoformat(),
                         ln.last.isoformat() if ln.last < dt.date(2026, 6, 30) else None, None])
        header = ["CUSIP", "TICKER", "GSUBIND", "INDFROM", "INDTHRU", "SNAPSHOTDATE"]
        p = csv_write(raw_dir / "gics_history.csv", header, rows)
        write_receipt(raw_dir, p, fetched_at=dt.datetime(2026, 7, 1, 6, 0), history_mode="pit_archive",
                      url="mock://gics/history", http_status=200)
        return [p]


ADAPTER = GicsAdapter()
