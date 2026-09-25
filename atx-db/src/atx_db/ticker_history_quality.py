"""Offline source diagnostics; internal agreement is not economic verification."""

from __future__ import annotations

import datetime as dt
from pathlib import Path
from typing import Any

import duckdb

SOURCE_PROVENANCE = {
    "adjusted_close_recipe": "finite_positive(close * cumulReturnFactor)",
    "split_factor": "unknown; returnFactor includes distributions and is not split-only",
    "availability_policy": "modeled tradingDate 22:00 backfill; not historical publication evidence",
    "historical_source_vintage": "unknown",
    "vendor_current_delivery": "05:00 America/Chicago T+1; historical vintage not authenticated",
    "identity_policy": "current-ticker CIK shortcut unverified; historical symbol is display only",
    "duplicate_policy": "quarantine all repeated positive vendor-security/date keys",
    "economic_adjustment_status": "unverified; diagnostics are internal consistency only",
}

_SOURCE_PROJECTION = """
SELECT try_cast(tradingDate AS DATE) AS trade_date,
       CASE WHEN regexp_full_match(trim(securityID::VARCHAR), '[+-]?[0-9]+')
            THEN try_cast(securityID AS BIGINT) END AS vendor_id,
       nullif(trim(securityID::VARCHAR), '') AS vendor_security_id,
       upper(trim(coalesce(nullif(ticker_tk, ''), nullif(todayTicker, '')))) AS symbol,
       upper(trim(coalesce(nullif(todayTicker, ''), nullif(ticker_tk, '')))) AS current_symbol,
       CASE WHEN regexp_full_match(trim(dn::VARCHAR), '[+-]?[0-9]+')
            THEN try_cast(dn AS BIGINT) END AS dn,
       try_cast(open AS DOUBLE) AS open,
       try_cast(high AS DOUBLE) AS high,
       try_cast(low AS DOUBLE) AS low,
       try_cast(close AS DOUBLE) AS close,
       try_cast(closePr AS DOUBLE) AS close_pr,
       try_cast(closeUnadjPr AS DOUBLE) AS close_unadj_pr,
       try_cast(volume AS BIGINT) AS volume,
       try_cast(shares AS BIGINT) AS shares_outstanding,
       try_cast(returnFactor AS DOUBLE) AS return_factor,
       try_cast(totalReturn AS DOUBLE) AS total_return,
       try_cast(cumulReturnFactor AS DOUBLE) AS cumul_return_factor
"""

_READERS = {
    "tsv": "read_csv(?, delim = '\t', header = true, all_varchar = true, "
           "auto_detect = true, sample_size = 20480)",
    "parquet": "read_parquet(?)",
}
_NUMERIC_COLUMNS = {
    "securityid", "dn", "open", "high", "low", "close", "closepr",
    "closeunadjpr", "volume", "shares", "returnfactor", "totalreturn", "cumulreturnfactor",
}
_IDENTIFIER_TYPES = {
    "TINYINT", "SMALLINT", "INTEGER", "BIGINT", "HUGEINT",
    "UTINYINT", "USMALLINT", "UINTEGER", "UBIGINT", "UHUGEINT",
}
_NUMERIC_TYPES = _IDENTIFIER_TYPES | {"FLOAT", "DOUBLE"}


def source_format(path: str | Path) -> str:
    """Identify an explicit local input format; ZIP extraction is separate."""
    suffix = Path(path).suffix.lower()
    if suffix in {".parquet", ".pq"}:
        return "parquet"
    if suffix in {".tsv", ".txt", ".tab"}:
        return "tsv"
    raise ValueError(f"unsupported ticker-history source suffix: {suffix!r}")


def source_provenance(path: str | Path) -> dict[str, str]:
    format_name = source_format(path)
    representation = (
        "native typed Parquet values; original text precision and source vintage unknown"
        if format_name == "parquet" else "TSV text parsed with DuckDB try_cast"
    )
    return {**SOURCE_PROVENANCE, "source_format": format_name,
            "source_numeric_representation": representation,
            "identifier_type_policy": "securityID/dn require native integer or strict integer text; "
                                      "native float/decimal columns rejected"}


def _validate_source_columns(con: duckdb.DuckDBPyConnection, reader: str, path: str) -> None:
    # DESCRIBE binds Parquet footer metadata, not its full row set. TSV is sampled.
    columns = {row[0].lower(): row[1] for row in con.execute(
        "DESCRIBE SELECT * FROM " + reader, [path]
    ).fetchall()}
    required = _NUMERIC_COLUMNS | {"tradingdate", "ticker_tk", "todayticker"}
    missing = sorted(required - columns.keys())
    if missing:
        raise ValueError("ticker-history source is missing required columns: " + ", ".join(missing))
    invalid = []
    for name in sorted(required):
        dtype = columns[name]
        valid = dtype == "VARCHAR"
        if name in {"securityid", "dn"}:
            # Reject unsupported schemas rather than silently invalidating even
            # integral floating/decimal values via their fractional text suffix.
            valid |= dtype in _IDENTIFIER_TYPES
        elif name in _NUMERIC_COLUMNS:
            valid |= dtype in _NUMERIC_TYPES or dtype.startswith("DECIMAL(")
        elif name == "tradingdate":
            valid |= dtype == "DATE" or dtype.startswith("TIMESTAMP")
        if not valid:
            invalid.append(f"{name}={dtype}")
    if invalid:
        raise ValueError("ticker-history source has incompatible column types: " + ", ".join(invalid))


def stage_source(con: duckdb.DuckDBPyConnection, path: str) -> None:
    """Retain all projected original rows, including rejected/duplicate neighbors."""
    reader = _READERS[source_format(path)]
    _validate_source_columns(con, reader, path)
    con.execute("CREATE OR REPLACE TEMP TABLE ticker_history_source_rows AS "
                + _SOURCE_PROJECTION + " FROM " + reader, [path])


def source_diagnostics(con: duckdb.DuckDBPyConnection) -> dict[str, Any]:
    """Measure before canonical filtering. Adjacency requires original unique dn."""
    con.execute("""
        CREATE OR REPLACE TEMP TABLE ticker_history_source_keys AS
        SELECT vendor_id, trade_date, count(*) AS key_rows
        FROM ticker_history_source_rows WHERE vendor_id > 0
        GROUP BY vendor_id, trade_date
    """)
    counts = con.execute("""
        SELECT count(*) AS source_rows,
               count(*) FILTER (WHERE vendor_id IS NULL) AS missing_or_invalid_id_rows,
               count(*) FILTER (WHERE vendor_id = 0) AS zero_id_rows,
               count(*) FILTER (WHERE vendor_id < 0) AS negative_id_rows,
               count(*) FILTER (WHERE trade_date IS NULL OR symbol IS NULL OR symbol = '') AS invalid_key_rows,
               count(*) FILTER (WHERE NOT coalesce(isfinite(close) AND close > 0, false)) AS invalid_close_rows,
               count(*) FILTER (WHERE NOT coalesce(isfinite(cumul_return_factor)
                    AND cumul_return_factor > 0, false)) AS invalid_cumulative_factor_rows,
               count(*) FILTER (WHERE NOT coalesce(isfinite(close * cumul_return_factor)
                    AND close * cumul_return_factor > 0 AND close > 0
                    AND cumul_return_factor > 0, false)) AS invalid_adjusted_product_rows,
               count(*) FILTER (WHERE volume < 0 OR open <= 0 OR high <= 0 OR low <= 0 OR close <= 0
                    OR NOT isfinite(open) OR NOT isfinite(high) OR NOT isfinite(low) OR NOT isfinite(close)
                    OR high < greatest(open, low, close) OR low > least(open, high, close)) AS invalid_ohlcv_rows,
               min(trade_date) AS first_date, max(trade_date) AS last_date
        FROM ticker_history_source_rows
    """).fetchdf().iloc[0].to_dict()
    duplicates = con.execute("""
        SELECT count(*) FILTER (WHERE key_rows > 1) AS repeated_positive_keys,
               coalesce(sum(key_rows) FILTER (WHERE key_rows > 1), 0) AS quarantined_positive_key_rows
        FROM ticker_history_source_keys
    """).fetchdf().iloc[0].to_dict()
    latest = con.execute("""
        SELECT count(*) AS latest_date_source_rows,
               count(DISTINCT vendor_id) FILTER (WHERE vendor_id > 0)
                   AS latest_date_distinct_positive_vendor_ids
        FROM ticker_history_source_rows
        WHERE trade_date = (SELECT max(trade_date) FROM ticker_history_source_rows)
    """).fetchdf().iloc[0].to_dict()
    # Duplicate predecessors remain in the sequence, blocking comparisons across them.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE ticker_history_pairs AS
        WITH grouped AS (
            SELECT k.vendor_id, k.trade_date, k.key_rows,
                   any_value(r.dn) AS dn, any_value(r.close) AS close,
                   any_value(r.close_pr) AS close_pr, any_value(r.close_unadj_pr) AS close_unadj_pr,
                   any_value(r.return_factor) AS return_factor, any_value(r.total_return) AS total_return,
                   any_value(r.cumul_return_factor) AS cumul_return_factor
            FROM ticker_history_source_keys k JOIN ticker_history_source_rows r
              ON r.vendor_id = k.vendor_id AND r.trade_date IS NOT DISTINCT FROM k.trade_date
            GROUP BY k.vendor_id, k.trade_date, k.key_rows
        )
        SELECT *, lag(key_rows) OVER w AS prior_key_rows, lag(dn) OVER w AS prior_dn,
               lag(trade_date) OVER w AS prior_date, lag(close) OVER w AS prior_close,
               lag(cumul_return_factor) OVER w AS prior_factor
        FROM grouped WINDOW w AS (PARTITION BY vendor_id ORDER BY trade_date)
    """)
    metrics: dict[str, Any] = {str(key): value for key, value in {**counts, **duplicates, **latest}.items()}
    adjacency = "key_rows = 1 AND prior_key_rows = 1 AND dn = prior_dn + 1 AND trade_date > prior_date"
    comparable = {
        "prior_close": ("close_unadj_pr / prior_close - 1", ["close_unadj_pr", "prior_close"]),
        "daily_factor": ("close_pr / close_unadj_pr - return_factor", ["close_pr", "close_unadj_pr", "return_factor"]),
        "reported_return": ("close / close_pr - 1 - total_return", ["close", "close_pr"]),
        "cumulative_recurrence": ("cumul_return_factor / prior_factor * return_factor - 1",
                                  ["cumul_return_factor", "prior_factor", "return_factor"]),
        "adjusted_return": ("(close * cumul_return_factor) / (prior_close * prior_factor) - 1 - total_return",
                            ["close", "cumul_return_factor", "prior_close", "prior_factor",
                             "close * cumul_return_factor", "prior_close * prior_factor"]),
    }
    selections = [f"count(*) FILTER (WHERE {adjacency}) AS adjacent_unique_pairs"]
    for label, (residual, fields) in comparable.items():
        valid = adjacency + " AND " + " AND ".join(f"isfinite({x}) AND {x} > 0" for x in fields)
        if label in {"reported_return", "adjusted_return"}:
            valid += " AND isfinite(total_return)"
        valid += f" AND isfinite({residual})"
        selections += [f"count(*) FILTER (WHERE {valid}) AS {label}_comparable",
                       f"count(*) FILTER (WHERE NOT coalesce({valid}, false)) AS {label}_not_comparable"]
        for exponent in (8, 6, 4, 3):
            selections.append(f"count(*) FILTER (WHERE {valid} AND abs({residual}) > 1e-{exponent}) "
                              f"AS {label}_residual_gt_1e_{exponent}")
    residual_counts = con.execute("SELECT " + ", ".join(selections) + " FROM ticker_history_pairs")
    metrics.update({str(key): value for key, value in residual_counts.fetchdf().iloc[0].to_dict().items()})
    con.execute("DROP TABLE ticker_history_pairs")
    return {key: _json_native(value) for key, value in metrics.items()}


def _json_native(value: Any) -> Any:
    """Plain JSON types: the diagnostics travel into stage details, run ledgers and quality checks.

    pandas hands back numpy scalars and Timestamps; source dates become ISO dates.
    """
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, dt.datetime):  # pandas Timestamp (and NaT, NaT != NaT) subclass datetime
        return None if value != value else value.date().isoformat()
    if isinstance(value, dt.date):
        return value.isoformat()
    if hasattr(value, "item"):
        return value.item()  # numpy scalar
    return value
