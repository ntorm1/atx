"""Native DuckDB bulk publication for the broad ticker-history archive.

The pandas chunk loader remains useful for small symbol subsets.  Full-universe
publication is different: repeatedly probing and deleting from a multi-million
row indexed table turns linear ingestion into an effectively quadratic job.
This module stages a projection of a local TSV or Parquet file, measures original-row
quality before exclusions, builds the replacement beside the live table,
validates it, and swaps it in atomically. Raw source bytes remain the lineage.

Daily date partitions after the full load go through
:mod:`atx_db.ticker_history_incremental` (P14), which reuses this module's
projection (``_RAW_CTE``), symbol map and :func:`vendor_share_unit_check`.
Incremental bars continue their line's stored factor through the vendor's
``returnFactor`` and are never rebased; they carry the ``run_id`` of the series
they extend. A later full republish replaces the source's rows in
``equity_daily_bars`` on the file's own basis, as one new ``run_id`` series:
each line's adjusted level moves by one constant (the vendor's own
recurrence), its ratios do not. The incremental path's rebase ledger
(``equity_adjustment_rebases``) and restated revisions
(``equity_daily_bar_revisions``) stay as history.
"""

from __future__ import annotations

import datetime as dt
import json
import logging
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path

from ._bulk_publication import publish_validated_shadow
from .connection import DuckDBStore
from .ticker_history import SOURCE_NAME, TBLTICKERHISTORY_ID_TYPE
from .ticker_history_quality import (
    SOURCE_PROVENANCE,
    source_diagnostics,
    source_format,
    source_provenance,
    stage_source,
)
from .warehouse import quality_check, record_source_file

LOGGER = logging.getLogger(__name__)

#: Unit of the vendor ``shares`` column by input format (A8, measured read-only):
#: ``TickerHistory3.parquet`` reports shares in THOUSANDS in every year 2012-2026
#: (AAPL run from 2026-07-17 = 14,594,180 vs its 10-Q cover 14,594,180,000; CELG
#: 2012 = 438,810; median positive shares per year 17.6K-29.5K), while the
#: ``tbltickerhistory3_10y`` TSV export reports UNITS (AAPL 2025-01-02 =
#: 15,204,137,000; NVDA 2024-07-05 = 24,598,341,970). ``equity_daily_bars``
#: always stores shares (units); ``BulkTickerHistoryOptions.shares_unit``
#: overrides the format default.
SHARES_UNIT_BY_FORMAT: dict[str, str] = {"parquet": "thousands", "tsv": "units"}
SHARES_UNIT_SCALE: dict[str, int] = {"units": 1, "thousands": 1000}
#: Plausible median of positive stored share counts on the latest date for a
#: real-size source (>= ``_UNIT_MAGNITUDE_MIN_ROWS`` lines): a unit mix-up moves
#: it by 1000x out of this band (units ~1e7; thousands mistaken for units ~2e4).
SHARES_MEDIAN_BAND: tuple[float, float] = (1e5, 1e9)
_UNIT_MAGNITUDE_MIN_ROWS = 1000
#: Stored vendor shares vs DEI EntityCommonStockSharesOutstanding on its cover date.
SHARES_DEI_RATIO_BAND: tuple[float, float] = (0.5, 2.0)
SHARES_UNIT_CHECK_NAME = "vendor_shares_unit"


@dataclass(frozen=True)
class BulkTickerHistoryOptions:
    # Retained as an input alias for existing callers and positional construction.
    tsv_path: Path | None = None
    source: str = SOURCE_NAME
    memory_limit: str = "1GB"
    threads: int = 1
    minimum_rows: int = 30_000_000
    minimum_securities: int = 10_000
    minimum_latest_date_securities: int = 5_000
    run_id: str | None = None
    source_path: Path | None = None
    #: Unit of the source ``shares`` column (``units``/``thousands``); None uses
    #: :data:`SHARES_UNIT_BY_FORMAT` for the input's format.
    shares_unit: str | None = None
    #: DEI-matched (line, cover date) pairs needed before a unit disagreement
    #: with DEI fails the publication (fewer pairs only flag it).
    shares_unit_min_pairs: int = 20

    def __post_init__(self) -> None:
        if (self.source_path is None) == (self.tsv_path is None):
            raise ValueError("supply exactly one source_path or legacy tsv_path")
        if self.shares_unit is not None and self.shares_unit not in SHARES_UNIT_SCALE:
            raise ValueError(f"shares_unit must be one of {sorted(SHARES_UNIT_SCALE)}, got {self.shares_unit!r}")
        if self.threads < 1:
            raise ValueError("threads must be positive")
        if min(
            self.minimum_rows,
            self.minimum_securities,
            self.minimum_latest_date_securities,
        ) < 1:
            raise ValueError("publication breadth floors must be positive")

    @property
    def input_path(self) -> Path:
        path = self.source_path if self.source_path is not None else self.tsv_path
        assert path is not None  # Validated in __post_init__.
        return path

    @property
    def resolved_shares_unit(self) -> str:
        return self.shares_unit or SHARES_UNIT_BY_FORMAT[source_format(self.input_path)]


@dataclass(frozen=True)
class BulkTickerHistoryResult:
    rows: int
    securities: int
    latest_date: dt.date
    latest_date_securities: int
    invalid_rows: int
    duplicate_keys: int
    elapsed_seconds: float
    run_id: str
    source_diagnostics: dict[str, object] = field(default_factory=dict)
    provenance: dict[str, str] = field(default_factory=lambda: dict(SOURCE_PROVENANCE))


_RAW_CTE = """
projected AS (
    SELECT r.*,
           CASE WHEN isfinite(close) AND close > 0
                     AND isfinite(cumul_return_factor) AND cumul_return_factor > 0
                     AND isfinite(close * cumul_return_factor) AND close * cumul_return_factor > 0
                THEN close * cumul_return_factor END AS adjusted_close,
           NULL::DOUBLE AS split_factor
    FROM ticker_history_source_rows r
    LEFT JOIN ticker_history_source_keys k
      ON r.vendor_id = k.vendor_id AND r.trade_date IS NOT DISTINCT FROM k.trade_date
    WHERE coalesce(k.key_rows, 1) = 1
),
raw AS (
    SELECT *
    FROM projected
    WHERE trade_date IS NOT NULL AND symbol IS NOT NULL AND symbol <> ''
)
"""


def _configure(store: DuckDBStore, options: BulkTickerHistoryOptions) -> None:
    temp_dir = options.input_path.parent / "duckdb-tmp"
    temp_dir.mkdir(parents=True, exist_ok=True)
    store.con.execute("PRAGMA disable_progress_bar")
    store.con.execute("SET memory_limit = ?", [options.memory_limit])
    store.con.execute("SET threads = ?", [options.threads])
    store.analytical_memory_limit = options.memory_limit
    store.analytical_threads = options.threads
    store.con.execute("SET preserve_insertion_order = false")
    store.con.execute("SET temp_directory = ?", [str(temp_dir)])


# Session-scoped staging this publisher creates. The source projection holds every source row.
_SESSION_TEMPORARIES = (
    "ticker_history_source_rows",
    "ticker_history_source_keys",
    "broad_symbol_map",
    "broad_line_map",
)


def _drop_session_temporaries(store: DuckDBStore) -> None:
    """Release this publisher's temporary tables once the bars are published.

    Left behind, they pin the whole source projection in memory for the rest of the
    session, and a later stage on the same connection (the activation ladder runs
    ``companyfacts_load`` next) refuses to recycle a connection that still holds
    caller-owned temporary objects.
    """
    for name in _SESSION_TEMPORARIES:
        store.con.execute(f"DROP TABLE IF EXISTS temp.main.{name}")


def _create_symbol_map(store: DuckDBStore) -> None:
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE broad_symbol_map AS
        WITH candidates AS (
            SELECT upper(trim(ticker)) AS symbol, security_id, 1 AS priority,
                   source_loaded_at, security_id AS tie_breaker
            FROM sec_company_tickers
            WHERE ticker IS NOT NULL AND trim(ticker) <> ''
            UNION ALL
            SELECT upper(trim(id_value)), security_id, 2, source_loaded_at, security_id
            FROM security_identifier_history
            WHERE id_type = 'TICKER'
              AND (valid_to IS NULL OR valid_to >= current_date)
            UNION ALL
            SELECT upper(trim(ticker)), security_id, 3, source_loaded_at, security_id
            FROM exchange_listings
            WHERE valid_to IS NULL OR valid_to >= current_date
        )
        SELECT symbol, security_id
        FROM candidates
        WHERE symbol IS NOT NULL AND symbol <> '' AND security_id IS NOT NULL
        QUALIFY row_number() OVER (
            PARTITION BY symbol
            ORDER BY priority, source_loaded_at DESC NULLS LAST, tie_breaker
        ) = 1
        """
    )


def _create_line_map(store: DuckDBStore, options: BulkTickerHistoryOptions) -> None:
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE broad_line_map AS
        WITH {_RAW_CTE},
        resolved AS (
            SELECT
                r.vendor_security_id,
                r.current_symbol AS symbol,
                r.trade_date,
                coalesce(
                    m.security_id,
                    CASE
                        WHEN r.vendor_security_id IS NULL OR r.vendor_security_id = '0'
                        THEN 'TBLTICKERHISTORY-SYMBOL-' || r.current_symbol || '-VENDOR-'
                             || coalesce(r.vendor_security_id, 'MISSING')
                        ELSE 'TBLTICKERHISTORY-' || r.vendor_security_id
                    END
                ) AS base_security_id
            FROM raw r
            LEFT JOIN broad_symbol_map m ON m.symbol = r.current_symbol
        ),
        line_stats AS (
            SELECT
                arg_max(base_security_id, (trade_date, symbol)) AS base_security_id,
                vendor_security_id,
                arg_max(symbol, (trade_date, symbol)) AS symbol,
                count(*) AS observations,
                min(trade_date) AS first_seen_date,
                max(trade_date) AS last_seen_date
            FROM resolved
            GROUP BY vendor_security_id,
                     CASE WHEN try_cast(vendor_security_id AS BIGINT) > 0 THEN '' ELSE symbol END
        ),
        ranked AS (
            SELECT *, row_number() OVER (
                PARTITION BY base_security_id
                ORDER BY observations DESC, vendor_security_id, symbol
            ) AS line_rank
            FROM line_stats
        )
        SELECT
            CASE
                WHEN line_rank = 1 THEN base_security_id
                WHEN try_cast(vendor_security_id AS BIGINT) > 0
                THEN 'TBLTICKERHISTORY-' || vendor_security_id
                ELSE 'TBLTICKERHISTORY-' || coalesce(vendor_security_id, '<NA>')
                     || '-' || symbol
            END AS security_id,
            vendor_security_id,
            symbol,
            CASE
                WHEN vendor_security_id IS NULL OR vendor_security_id = '0'
                THEN 'SYMBOL-' || symbol || '-VENDOR-'
                     || coalesce(vendor_security_id, 'MISSING')
                ELSE vendor_security_id
            END AS identifier_value,
            observations,
            first_seen_date,
            last_seen_date
        FROM ranked
        """,
    )


def _create_next_table(store: DuckDBStore, options: BulkTickerHistoryOptions) -> None:
    con = store.con
    scale = SHARES_UNIT_SCALE[options.resolved_shares_unit]
    con.execute("DROP TABLE IF EXISTS equity_daily_bars_bulk_next")
    con.execute(
        """
        CREATE TABLE equity_daily_bars_bulk_next (
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            vendor_security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            trade_date DATE NOT NULL,
            open DOUBLE,
            high DOUBLE,
            low DOUBLE,
            close DOUBLE,
            adjusted_close DOUBLE,
            volume BIGINT,
            vwap DOUBLE,
            dividend_amount DOUBLE,
            split_factor DOUBLE,
            is_adjusted BOOLEAN NOT NULL DEFAULT false,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            as_of_date DATE,
            is_latest_revision BOOLEAN,
            shares_outstanding BIGINT,
            market_cap_usd DOUBLE
        )
        """
    )
    # Preserve every non-replaced source in the complete physical shadow.  The
    # later rename then replaces the public table as one catalog transaction,
    # rather than deleting 32M indexed live rows before an insert can commit.
    con.execute(
        """
        INSERT INTO equity_daily_bars_bulk_next
        SELECT * FROM equity_daily_bars WHERE source <> ?
        """,
        [options.source],
    )
    con.execute(
        f"""
        INSERT INTO equity_daily_bars_bulk_next
        WITH {_RAW_CTE},
        valid AS (
            SELECT r.*, m.security_id
            FROM raw r
            JOIN broad_line_map m
              ON m.vendor_security_id IS NOT DISTINCT FROM r.vendor_security_id
             AND (r.vendor_id > 0 OR m.symbol = r.current_symbol)
            WHERE NOT (
                coalesce(r.volume < 0, false)
                OR coalesce(r.open <= 0, false)
                OR coalesce(r.high <= 0, false)
                OR coalesce(r.low <= 0, false)
                OR coalesce(r.close <= 0, false)
                OR coalesce(NOT isfinite(r.open), false)
                OR coalesce(NOT isfinite(r.high), false)
                OR coalesce(NOT isfinite(r.low), false)
                OR coalesce(NOT isfinite(r.close), false)
                OR coalesce(r.high < greatest(r.open, r.low, r.close), false)
                OR coalesce(r.low > least(r.open, r.high, r.close), false)
            )
        )
        SELECT
            ?, security_id, vendor_security_id, symbol, trade_date,
            open, high, low, close, adjusted_close, volume,
            NULL::DOUBLE, NULL::DOUBLE, split_factor, false,
            trade_date::TIMESTAMP + INTERVAL 22 HOUR,
            ?, current_timestamp, NULL::DATE, NULL::BOOLEAN,
            -- A8: stored in shares (units) whatever the source unit.
            shares_outstanding * ?, shares_outstanding * ? * close
        FROM valid
        QUALIFY count(*) OVER (PARTITION BY security_id, trade_date) = 1
        """,
        [options.source, options.run_id, scale, scale],
    )


def _validate_next(
    store: DuckDBStore, options: BulkTickerHistoryOptions
) -> tuple[int, int, dt.date, int, int, int]:
    row = store.con.execute(
        """
        SELECT count(*), count(DISTINCT security_id), min(trade_date), max(trade_date)
        FROM equity_daily_bars_bulk_next
        WHERE source = ?
        """,
        [options.source],
    ).fetchone()
    if row is None or row[3] is None:
        raise RuntimeError("bulk ticker-history stage is empty")
    rows, securities, _minimum_date, latest_date = row
    latest_row = store.con.execute(
        """
        SELECT count(DISTINCT security_id)
        FROM equity_daily_bars_bulk_next
        WHERE source = ? AND trade_date = ?
        """,
        [options.source, latest_date],
    ).fetchone()
    if latest_row is None:
        raise RuntimeError("could not validate latest-date ticker-history breadth")
    latest_securities = latest_row[0]
    duplicate_row = store.con.execute(
        """
        SELECT count(*)
        FROM (
            SELECT security_id, trade_date
            FROM equity_daily_bars_bulk_next
            WHERE source = ?
            GROUP BY security_id, trade_date
            HAVING count(*) > 1
        )
        """,
        [options.source],
    ).fetchone()
    if duplicate_row is None:
        raise RuntimeError("could not validate ticker-history duplicate keys")
    duplicate_keys = duplicate_row[0]
    invalid_row = store.con.execute(
        """
        SELECT count(*)
        FROM equity_daily_bars_bulk_next
        WHERE source = ? AND (
            volume < 0 OR open <= 0 OR high <= 0 OR low <= 0 OR close <= 0
            OR high < greatest(open, low, close)
            OR low > least(open, high, close)
        )
        """,
        [options.source],
    ).fetchone()
    if invalid_row is None:
        raise RuntimeError("could not validate ticker-history OHLCV")
    invalid_rows = invalid_row[0]
    failures = []
    if rows < options.minimum_rows:
        failures.append(f"rows {rows:,} < {options.minimum_rows:,}")
    if securities < options.minimum_securities:
        failures.append(f"securities {securities:,} < {options.minimum_securities:,}")
    if latest_securities < options.minimum_latest_date_securities:
        failures.append(
            f"latest-date securities {latest_securities:,} < "
            f"{options.minimum_latest_date_securities:,}"
        )
    if duplicate_keys:
        failures.append(f"duplicate keys {duplicate_keys:,}")
    if invalid_rows:
        failures.append(f"invalid OHLCV rows {invalid_rows:,}")
    if failures:
        raise RuntimeError("bulk ticker-history publication gate failed: " + "; ".join(failures))
    return (
        int(rows),
        int(securities),
        latest_date,
        int(latest_securities),
        int(invalid_rows),
        int(duplicate_keys),
    )


def _check_share_units(store: DuckDBStore, options: BulkTickerHistoryOptions) -> dict[str, object]:
    """The A8 share-unit check on the bulk shadow table (see :func:`vendor_share_unit_check`)."""
    return vendor_share_unit_check(
        store,
        table="equity_daily_bars_bulk_next",
        source=options.source,
        run_id=options.run_id,
        unit=options.resolved_shares_unit,
        min_pairs=options.shares_unit_min_pairs,
    )


def vendor_share_unit_check(
    store: DuckDBStore, *, table: str, source: str, run_id: str | None, unit: str, min_pairs: int
) -> dict[str, object]:
    """Sanity-check the stored (scaled) vendor share counts before publication (A8).

    ``table`` is a staged relation with the ``equity_daily_bars`` columns
    ``source, security_id, trade_date, shares_outstanding`` (the bulk shadow
    table, or an incremental partition's staged rows). Two independent checks:

    * magnitude: the median positive share count on the latest date must lie in
      :data:`SHARES_MEDIAN_BAND` once at least ``_UNIT_MAGNITUDE_MIN_ROWS`` lines
      carry shares (a 1000x unit error moves it out of the band);
    * DEI: for lines whose CIK has exactly one SEC ticker, the first bar on or
      within four days after a dei:EntityCommonStockSharesOutstanding cover date
      (the vendor starts its share run on that date) is compared with the DEI
      count; the median ratio must lie in :data:`SHARES_DEI_RATIO_BAND`.

    A conclusive disagreement (enough rows or ``min_pairs`` pairs) records a
    failed ``vendor_shares_unit`` check and raises before anything is published;
    a disagreement on fewer pairs is flagged as a warning, as is a run with no
    conclusive evidence either way.
    """
    magnitude = store.con.execute(
        f"""
        SELECT count(*), median(shares_outstanding)
        FROM {table}
        WHERE source = ? AND shares_outstanding > 0
          AND trade_date = (SELECT max(trade_date) FROM {table} WHERE source = ?)
        """,
        [source, source],
    ).fetchone()
    latest_rows, latest_median = (int(magnitude[0]), magnitude[1]) if magnitude else (0, None)
    dei = store.con.execute(
        f"""
        WITH single_ciks AS (
            SELECT try_cast(cik AS BIGINT) AS cik
            FROM sec_company_tickers
            GROUP BY 1
            HAVING count(DISTINCT upper(trim(ticker))) = 1
        ),
        covers AS (
            SELECT t.security_id, d.effective_date, max(d.share_count) AS dei_shares
            FROM shares_outstanding_history d
            JOIN single_ciks c ON c.cik = try_cast(d.cik AS BIGINT)
            JOIN sec_company_tickers t ON try_cast(t.cik AS BIGINT) = c.cik
            WHERE d.taxonomy = 'dei' AND d.concept = 'EntityCommonStockSharesOutstanding'
              AND d.share_class IS NULL AND d.share_count > 0
            GROUP BY ALL
            HAVING count(DISTINCT d.share_count) = 1
        ),
        candidates AS (
            SELECT c.*, o.lag_days, c.effective_date + CAST(o.lag_days AS INTEGER) AS trade_date
            FROM covers c CROSS JOIN range(0, 5) AS o(lag_days)
        ),
        pairs AS (
            SELECT c.security_id, c.effective_date,
                   arg_min(b.shares_outstanding / c.dei_shares, c.lag_days) AS ratio
            FROM candidates c
            JOIN {table} b
              ON b.security_id = c.security_id AND b.trade_date = c.trade_date
            WHERE b.source = ? AND b.shares_outstanding > 0
            GROUP BY ALL
        )
        SELECT count(*), median(ratio) FROM pairs
        """,
        [source],
    ).fetchone()
    dei_pairs, dei_median = (int(dei[0]), dei[1]) if dei else (0, None)
    low, high = SHARES_DEI_RATIO_BAND
    magnitude_low, magnitude_high = SHARES_MEDIAN_BAND
    magnitude_conclusive = latest_rows >= _UNIT_MAGNITUDE_MIN_ROWS and latest_median is not None
    magnitude_ok = not magnitude_conclusive or magnitude_low <= latest_median <= magnitude_high
    dei_ok = dei_median is None or low <= dei_median <= high
    dei_conclusive = dei_pairs >= min_pairs
    failures = []
    if not magnitude_ok:
        failures.append(f"latest-date median shares {latest_median:,.0f} outside {SHARES_MEDIAN_BAND}")
    if dei_conclusive and not dei_ok:
        failures.append(f"median vendor/DEI share ratio {dei_median:.4g} over {dei_pairs:,} pairs outside {low}-{high}")
    if failures:
        status = "failed"
    elif dei_ok and (magnitude_conclusive or dei_conclusive):
        status = "passed"
    else:
        status = "warning"
    details: dict[str, object] = {
        "run_id": run_id,
        "shares_unit": unit,
        "shares_scale": SHARES_UNIT_SCALE[unit],
        "latest_date_share_rows": latest_rows,
        "latest_date_median_shares": latest_median,
        "median_band": list(SHARES_MEDIAN_BAND),
        "dei_pairs": dei_pairs,
        "dei_median_ratio": dei_median,
        "dei_ratio_band": list(SHARES_DEI_RATIO_BAND),
        "dei_min_pairs": min_pairs,
        "failures": failures,
    }
    quality_check(
        store,
        dataset_id="tbltickerhistory_daily",
        table_name="equity_daily_bars",
        check_name=SHARES_UNIT_CHECK_NAME,
        status=status,
        severity="error" if failures else "warning",
        observed_value=dei_median,
        threshold_value=1.0,
        details=details,
    )
    if failures:
        raise RuntimeError(f"vendor share unit {unit!r} check failed: " + "; ".join(failures))
    return details


def _publish(store: DuckDBStore, options: BulkTickerHistoryOptions) -> None:
    con = store.con
    def publish_links() -> None:
        con.execute(
            """
            UPDATE securities AS s SET
                primary_symbol = m.symbol,
                name = coalesce(s.name, m.symbol),
                first_seen_date = least(s.first_seen_date, m.first_seen_date),
                last_seen_date = greatest(s.last_seen_date, m.last_seen_date),
                active = true
            FROM broad_line_map m
            WHERE s.security_id = m.security_id AND s.source = ?
            """,
            [options.source],
        )
        con.execute(
            """
            INSERT INTO securities (
                security_id, issuer_id, primary_symbol, name, asset_class,
                country, currency, active, first_seen_date, last_seen_date, source
            )
            SELECT security_id, NULL, symbol, symbol, 'EQUITY', 'US', 'USD', true,
                   first_seen_date, last_seen_date, ?
            FROM broad_line_map m
            WHERE NOT EXISTS (
                SELECT 1 FROM securities s WHERE s.security_id = m.security_id
            )
            """,
            [options.source],
        )
        con.execute(
            "DELETE FROM security_identifier_history WHERE source = ? AND id_type = ?",
            [options.source, TBLTICKERHISTORY_ID_TYPE],
        )
        con.execute(
            """
            INSERT INTO security_identifier_history (
                security_id, id_type, id_value, valid_from, valid_to, as_of_date,
                available_at, source, run_id
            )
            SELECT security_id, ?, identifier_value, first_seen_date, NULL,
                   first_seen_date, current_timestamp,
                   ?, ?
            FROM broad_line_map
            """,
            [TBLTICKERHISTORY_ID_TYPE, options.source, options.run_id],
        )
        con.execute("DELETE FROM exchange_listings WHERE source = ?", [options.source])
        con.execute(
            """
            INSERT INTO exchange_listings (
                security_id, ticker, exchange_code, mic, currency, valid_from,
                valid_to, as_of_date, available_at, source, run_id
            )
            SELECT security_id, symbol, NULL, NULL, 'USD', first_seen_date, NULL,
                   first_seen_date, current_timestamp,
                   ?, ?
            FROM broad_line_map
            """,
            [options.source, options.run_id],
        )
    publish_validated_shadow(
        store,
        live_table="equity_daily_bars",
        shadow_table="equity_daily_bars_bulk_next",
        before_swap=publish_links,
    )


def _record_failed_publication(store: DuckDBStore, run_id: str, error: Exception) -> None:
    """Best-effort failure ledgering without masking the publication error."""
    for recover in (False, True):
        try:
            if recover:
                store.recover_failed_connection()
            store.con.execute(
                """
                UPDATE dataset_runs SET status = 'failed', finished_at = current_timestamp,
                                        error_message = ?
                WHERE run_id = ?
                """,
                [str(error), run_id],
            )
            return
        except Exception as ledger_error:
            if recover:
                note = (
                    f"Operator recovery required for dataset_runs run_id={run_id!r}: "
                    f"could not record the original publication failure: {ledger_error}"
                )
                error.add_note(note)
                LOGGER.error("%s", note)


def publish_bulk_ticker_history(
    store: DuckDBStore, options: BulkTickerHistoryOptions
) -> BulkTickerHistoryResult:
    if not options.input_path.is_file():
        raise FileNotFoundError(options.input_path)
    # The resolved unit is part of the run's params_json and provenance, so every
    # stored share count names the unit it was scaled from (A8).
    options = BulkTickerHistoryOptions(**{**asdict(options), "shares_unit": options.resolved_shares_unit})
    provenance = {
        **source_provenance(options.input_path),
        "shares_unit": options.resolved_shares_unit,
        "shares_scale": str(SHARES_UNIT_SCALE[options.resolved_shares_unit]),
        "shares_outstanding_unit": "shares",
    }
    started = time.perf_counter()
    run_id = options.run_id or f"broad-bars-bulk-{uuid.uuid4()}"
    options = BulkTickerHistoryOptions(**{**asdict(options), "run_id": run_id})
    _configure(store, options)
    store.con.execute(
        """
        INSERT OR REPLACE INTO dataset_runs (
            run_id, dataset_id, status, started_at, source, params_json
        )
        VALUES (?, 'tbltickerhistory_daily', 'running', current_timestamp, ?, ?)
        """,
        [run_id, options.source, json.dumps(asdict(options), default=str, sort_keys=True)],
    )
    try:
        record_source_file(
            store,
            dataset_id="tbltickerhistory_daily",
            source_url=str(options.input_path),
            cache_path=options.input_path,
            status="available",
            metadata={"mode": "native_bulk_projection", "run_id": run_id, **provenance},
            compute_hash=True,
        )
        stage_source(store.con, str(options.input_path))
        diagnostics = source_diagnostics(store.con)
        LOGGER.warning("ticker-history source diagnostics: %s; provenance: %s", diagnostics, provenance)
        quality_check(
            store, dataset_id="tbltickerhistory_daily", table_name="equity_daily_bars",
            check_name="source_preprojection_diagnostics", status="warning",
            observed_value=float(diagnostics["quarantined_positive_key_rows"]), threshold_value=0.0,
            details={"run_id": run_id, "diagnostics": diagnostics, "provenance": provenance},
        )
        _create_symbol_map(store)
        identity_counts = store.con.execute(
            "SELECT count(*) FILTER (WHERE m.security_id IS NOT NULL), "
            "count(*) FILTER (WHERE m.security_id IS NULL) "
            "FROM ticker_history_source_rows r LEFT JOIN broad_symbol_map m ON m.symbol = r.current_symbol"
        ).fetchone()
        if identity_counts is not None:
            diagnostics["current_symbol_linked_unverified_rows"] = identity_counts[0]
            diagnostics["no_current_symbol_identity_match_rows"] = identity_counts[1]
        LOGGER.info("built canonical symbol map")
        _create_line_map(store, options)
        LOGGER.info("built vendor-line collision map")
        _create_next_table(store, options)
        LOGGER.info("built canonical daily-bar staging table")
        metrics = _validate_next(store, options)
        LOGGER.info(
            "validated %d rows across %d securities; latest breadth %d",
            metrics[0],
            metrics[1],
            metrics[3],
        )
        share_units = _check_share_units(store, options)
        LOGGER.info("vendor share unit check: %s", share_units)
        _publish(store, options)
        LOGGER.info("atomically published bars and canonical indexes")
    except Exception as exc:
        # Keep staging for inspection. Cleanup must not invalidate a recovered
        # connection or hide the original failure; the next explicit run replaces it.
        _record_failed_publication(store, run_id, exc)
        raise
    _drop_session_temporaries(store)
    rows, securities, latest_date, latest_securities, invalid_rows, duplicate_keys = metrics
    elapsed = time.perf_counter() - started
    store.con.execute(
        """
        UPDATE dataset_runs SET status = 'succeeded', finished_at = current_timestamp,
                                rows_loaded = ?
        WHERE run_id = ?
        """,
        [rows, run_id],
    )
    quality_check(
        store,
        dataset_id="tbltickerhistory_daily",
        table_name="equity_daily_bars",
        check_name="institutional_latest_date_breadth",
        status="passed",
        observed_value=float(latest_securities),
        threshold_value=float(options.minimum_latest_date_securities),
        details={
            "run_id": run_id,
            "rows": rows,
            "securities": securities,
            "latest_date": latest_date,
            "duplicate_keys": duplicate_keys,
            "invalid_rows": invalid_rows,
            "source_diagnostics": diagnostics,
            "provenance": provenance,
            "share_units": share_units,
        },
    )
    store.con.execute("CHECKPOINT")
    return BulkTickerHistoryResult(
        rows=rows,
        securities=securities,
        latest_date=latest_date,
        latest_date_securities=latest_securities,
        invalid_rows=invalid_rows,
        duplicate_keys=duplicate_keys,
        elapsed_seconds=elapsed,
        run_id=run_id,
        source_diagnostics=diagnostics,
        provenance=provenance,
    )
