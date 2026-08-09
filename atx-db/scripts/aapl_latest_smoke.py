#!/usr/bin/env python
from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import duckdb
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore


SURFACE_OBJECTS = (
    "v_alpha_daily_panel",
    "equity_daily_bars",
    "daily_adjustment_factors",
    "equity_price_metrics",
    "fundamental_ratios",
    "fundamental_xbrl_metric",
    "fundamental_statement_points",
    "fundamental_ttm_points",
    "fundamental_periods",
    "fundamental_growth",
    "valuation_multiples",
    "fundamental_factor_values",
    "cross_domain_factor_values",
    "v_factor_panel",
    "v_factor_panel_wide",
    "short_interest_metrics",
    "short_volume_metrics",
    "thirteenf_concentration_metrics",
    "thirteenf_option_metrics",
    "thirteenf_position_metrics",
    "corporate_action_dividend_metrics",
    "corporate_action_split_metrics",
    "corporate_action_factor_reconciliation",
    "insider_transaction_metrics",
    "security_listing_metrics",
    "offexchange_volume",
    "offexchange_security_period",
)


@dataclass(frozen=True)
class ObjectInfo:
    name: str
    columns: tuple[str, ...]

    def has(self, *names: str) -> bool:
        available = set(self.columns)
        return all(name in available for name in names)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Print a latest-trading-date warehouse smoke snapshot for one symbol.",
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument(
        "--as-of-date",
        type=dt.date.fromisoformat,
        help="Override the latest trading date auto-detected from AAPL daily data.",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=80,
        help="Rows to print per section. Use 0 for all rows.",
    )
    parser.add_argument(
        "--max-col-width",
        type=int,
        default=120,
        help="Maximum rendered column width for long JSON/text fields.",
    )
    parser.add_argument(
        "--initialize-schema",
        action="store_true",
        help="Apply pending warehouse migrations before reading. Default is read-only.",
    )
    return parser.parse_args()


def _end_of_day(value: dt.date) -> dt.datetime:
    return dt.datetime.combine(value, dt.time(23, 59, 59, 999999))


def _safe_ident(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value):
        raise ValueError(f"unsafe SQL identifier: {value!r}")
    return value


def _object_info(con: duckdb.DuckDBPyConnection, object_name: str) -> ObjectInfo | None:
    rows = con.execute(
        """
        SELECT column_name
        FROM duckdb_columns()
        WHERE schema_name = 'main'
          AND table_name = ?
        ORDER BY column_index
        """,
        [object_name],
    ).fetchall()
    if not rows:
        return None
    return ObjectInfo(name=object_name, columns=tuple(str(row[0]) for row in rows))


def _load_objects(con: duckdb.DuckDBPyConnection) -> dict[str, ObjectInfo]:
    objects: dict[str, ObjectInfo] = {}
    for object_name in SURFACE_OBJECTS + ("v_security_master_current", "security_identifier_history", "v_metric_lineage"):
        info = _object_info(con, object_name)
        if info is not None:
            objects[object_name] = info
    return objects


def _symbol_predicate(info: ObjectInfo, security_ids: tuple[str, ...] = ()) -> tuple[str, list[Any]]:
    if "symbol" in info.columns:
        return "upper(symbol) = upper(?)", []
    if "issuer_trading_symbol" in info.columns:
        return "upper(issuer_trading_symbol) = upper(?)", []
    if "primary_symbol" in info.columns:
        return "upper(primary_symbol) = upper(?)", []
    if security_ids and "security_id" in info.columns:
        placeholders = ", ".join("?" for _ in security_ids)
        return f"security_id IN ({placeholders})", list(security_ids)
    return "", []


def _latest_date_from(
    con: duckdb.DuckDBPyConnection,
    objects: dict[str, ObjectInfo],
    symbol: str,
    explicit: dt.date | None,
) -> dt.date:
    if explicit is not None:
        return explicit

    candidates: list[dt.date] = []
    for object_name, date_col in (
        ("v_alpha_daily_panel", "as_of_date"),
        ("equity_daily_bars", "trade_date"),
        ("equity_price_metrics", "trade_date"),
    ):
        info = objects.get(object_name)
        if info is None or not info.has(date_col):
            continue
        predicate, extra_params = _symbol_predicate(info)
        if not predicate:
            continue
        row = con.execute(
            f"SELECT max({_safe_ident(date_col)}) FROM {_safe_ident(object_name)} WHERE {predicate}",
            [symbol, *extra_params],
        ).fetchone()
        if row is not None and row[0] is not None:
            candidates.append(row[0])

    if not candidates:
        raise RuntimeError(f"No trading date found for symbol {symbol!r} in daily panel, bars, or price metrics.")
    return max(candidates)


def _security_ids_for_symbol(
    con: duckdb.DuckDBPyConnection,
    objects: dict[str, ObjectInfo],
    symbol: str,
) -> tuple[str, ...]:
    candidates = (
        "v_alpha_daily_panel",
        "equity_daily_bars",
        "equity_price_metrics",
        "fundamental_ratios",
        "fundamental_xbrl_metric",
        "v_security_master_current",
    )
    security_ids: set[str] = set()
    for object_name in candidates:
        info = objects.get(object_name)
        if info is None or "security_id" not in info.columns:
            continue
        predicate, extra_params = _symbol_predicate(info)
        if not predicate:
            continue
        rows = con.execute(
            f"""
            SELECT DISTINCT security_id
            FROM {_safe_ident(object_name)}
            WHERE {predicate}
              AND security_id IS NOT NULL
            """,
            [symbol, *extra_params],
        ).fetchall()
        security_ids.update(str(row[0]) for row in rows if row[0] is not None)
    return tuple(sorted(security_ids))


def _fetch(con: duckdb.DuckDBPyConnection, sql: str, params: Iterable[Any] = ()) -> pd.DataFrame:
    return con.execute(sql, list(params)).fetchdf()


def _render_frame(frame: pd.DataFrame, *, limit: int, max_col_width: int) -> str:
    if frame.empty:
        return "(no rows)"
    shown = frame if limit <= 0 else frame.head(limit)
    with pd.option_context("display.max_colwidth", max_col_width, "display.width", 240):
        rendered = shown.to_string(index=False)
    if limit > 0 and len(frame) > limit:
        rendered += f"\n... {len(frame) - limit} more row(s); rerun with --limit 0 to print all."
    return rendered


def _print_section(
    con: duckdb.DuckDBPyConnection,
    title: str,
    sql: str,
    params: Iterable[Any] = (),
    *,
    limit: int,
    max_col_width: int,
) -> None:
    print(f"\n-- {title}")
    try:
        frame = _fetch(con, sql, params)
    except Exception as exc:
        print(f"(error: {type(exc).__name__}: {exc})")
        return
    print(f"rows: {len(frame)}")
    print(_render_frame(frame, limit=limit, max_col_width=max_col_width))


def _print_missing(title: str, object_name: str) -> None:
    print(f"\n-- {title}")
    print(f"(missing object: {object_name})")


def _coverage_frame(
    con: duckdb.DuckDBPyConnection,
    objects: dict[str, ObjectInfo],
    symbol: str,
    security_ids: tuple[str, ...],
) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []
    date_candidates = ("trade_date", "as_of_date", "period_end", "settlement_date", "report_period", "ex_date")
    for object_name in SURFACE_OBJECTS:
        info = objects.get(object_name)
        if info is None:
            rows.append({"object_name": object_name, "status": "missing"})
            continue

        predicate, extra_params = _symbol_predicate(info, security_ids)
        if not predicate:
            rows.append({"object_name": object_name, "status": "present_unfiltered", "columns": len(info.columns)})
            continue

        date_col = next((column for column in date_candidates if column in info.columns), None)
        available_expr = "max(available_at)" if "available_at" in info.columns else "NULL"
        if date_col is not None:
            date_expr = f"min({_safe_ident(date_col)}), max({_safe_ident(date_col)})"
        else:
            date_expr = "NULL, NULL"
        row = con.execute(
            f"""
            SELECT count(*)::BIGINT AS rows, {date_expr}, {available_expr}
            FROM {_safe_ident(object_name)}
            WHERE {predicate}
            """,
            [symbol, *extra_params] if "?" in predicate and "security_id IN" not in predicate else extra_params,
        ).fetchone()
        rows.append(
            {
                "object_name": object_name,
                "status": "present",
                "rows": int(row[0]) if row is not None and row[0] is not None else 0,
                "min_date": None if row is None else row[1],
                "max_date": None if row is None else row[2],
                "max_available_at": None if row is None else row[3],
                "columns": len(info.columns),
            }
        )
    return pd.DataFrame(rows)


def _where_symbol_or_security(
    info: ObjectInfo,
    symbol: str,
    security_ids: tuple[str, ...],
) -> tuple[str, list[Any]]:
    predicate, extra = _symbol_predicate(info, security_ids)
    if not predicate:
        return "1 = 0", []
    if "security_id IN" in predicate:
        return predicate, extra
    return predicate, [symbol]


def _maybe_section(
    con: duckdb.DuckDBPyConnection,
    objects: dict[str, ObjectInfo],
    object_name: str,
    title: str,
    sql: str,
    params: Iterable[Any],
    *,
    limit: int,
    max_col_width: int,
) -> None:
    if object_name not in objects:
        _print_missing(title, object_name)
        return
    _print_section(con, title, sql, params, limit=limit, max_col_width=max_col_width)


def main() -> int:
    args = parse_args()
    symbol = args.symbol.strip().upper()

    if args.initialize_schema:
        with DuckDBStore(args.db_path):
            pass

    con = duckdb.connect(str(args.db_path), read_only=True)
    try:
        objects = _load_objects(con)
        latest_date = _latest_date_from(con, objects, symbol, args.as_of_date)
        as_of_ts = _end_of_day(latest_date)
        security_ids = _security_ids_for_symbol(con, objects, symbol)

        migration_row = con.execute(
            """
            SELECT max(CAST(version AS INTEGER)) FILTER (WHERE version ~ '^[0-9]+$') AS max_version,
                   count(*) AS migration_rows
            FROM schema_migrations
            """
        ).fetchone()

        print(f"Warehouse smoke snapshot for {symbol}")
        print(f"db_path: {args.db_path}")
        print(f"latest_trading_date: {latest_date}")
        print(f"as_of_ts: {as_of_ts.isoformat(sep=' ')}")
        print(f"security_ids: {', '.join(security_ids) if security_ids else '(none found)'}")
        print(f"schema_migrations: max_version={migration_row[0]}, rows={migration_row[1]}")
        print(f"render_limit_per_section: {'all' if args.limit <= 0 else args.limit}")

        coverage = _coverage_frame(con, objects, symbol, security_ids)
        print("\n-- surface coverage")
        print(_render_frame(coverage, limit=0, max_col_width=args.max_col_width))

        if "v_security_master_current" in objects:
            info = objects["v_security_master_current"]
            predicate, params = _where_symbol_or_security(info, symbol, security_ids)
            _print_section(
                con,
                "security master current",
                f"SELECT * FROM v_security_master_current WHERE {predicate}",
                params,
                limit=args.limit,
                max_col_width=args.max_col_width,
            )
        else:
            _print_missing("security master current", "v_security_master_current")

        if "security_identifier_history" in objects and security_ids:
            placeholders = ", ".join("?" for _ in security_ids)
            _print_section(
                con,
                "security identifier history",
                f"""
                SELECT *
                FROM security_identifier_history
                WHERE security_id IN ({placeholders})
                  AND as_of_date <= ?
                  AND available_at <= ?
                ORDER BY id_type, valid_from DESC, available_at DESC
                """,
                [*security_ids, latest_date, as_of_ts],
                limit=args.limit,
                max_col_width=args.max_col_width,
            )

        _maybe_section(
            con,
            objects,
            "v_alpha_daily_panel",
            "daily panel latest",
            """
            SELECT *
            FROM v_alpha_daily_panel
            WHERE upper(symbol) = upper(?)
              AND as_of_date = ?
            ORDER BY security_id
            """,
            [symbol, latest_date],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "equity_daily_bars",
            "raw daily bars latest",
            """
            SELECT *
            FROM equity_daily_bars
            WHERE upper(symbol) = upper(?)
              AND trade_date = ?
            ORDER BY source, available_at DESC, source_loaded_at DESC
            """,
            [symbol, latest_date],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "equity_daily_bars",
            "raw bar trailing stats",
            """
            WITH deduped AS (
                SELECT *
                FROM equity_daily_bars
                WHERE upper(symbol) = upper(?)
                  AND trade_date <= ?
                QUALIFY row_number() OVER (
                    PARTITION BY trade_date
                    ORDER BY available_at DESC, source_loaded_at DESC, source DESC
                ) = 1
            ),
            tail AS (
                SELECT *
                FROM deduped
                ORDER BY trade_date DESC
                LIMIT 252
            ),
            ordered AS (
                SELECT
                    *,
                    first_value(close) OVER (
                        ORDER BY trade_date
                        ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                    ) AS first_close,
                    last_value(close) OVER (
                        ORDER BY trade_date
                        ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                    ) AS last_close
                FROM tail
            )
            SELECT
                count(*)::BIGINT AS trading_days,
                min(trade_date) AS first_trade_date,
                max(trade_date) AS last_trade_date,
                any_value(first_close) AS first_close,
                any_value(last_close) AS last_close,
                any_value(last_close) / nullif(any_value(first_close), 0) - 1 AS price_return,
                min(close) AS min_close,
                max(close) AS max_close,
                avg(close) AS avg_close,
                avg(volume) AS avg_volume,
                avg(close * volume) AS avg_dollar_volume
            FROM ordered
            """,
            [symbol, latest_date],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "daily_adjustment_factors",
            "daily adjustment factors latest",
            """
            SELECT *
            FROM daily_adjustment_factors
            WHERE upper(symbol) = upper(?)
              AND trade_date = ?
              AND as_of_date <= ?
              AND available_at <= ?
            ORDER BY source, available_at DESC, source_loaded_at DESC
            """,
            [symbol, latest_date, latest_date, as_of_ts],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "equity_price_metrics",
            "equity price metrics latest",
            """
            SELECT *
            FROM equity_price_metrics
            WHERE upper(symbol) = upper(?)
              AND trade_date = ?
              AND as_of_date <= ?
              AND available_at <= ?
              AND is_latest_revision
            ORDER BY source, available_at DESC, source_loaded_at DESC
            """,
            [symbol, latest_date, latest_date, as_of_ts],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "fundamental_ratios",
            "fundamental ratios latest per ratio",
            """
            WITH visible AS (
                SELECT *
                FROM fundamental_ratios
                WHERE upper(symbol) = upper(?)
                  AND as_of_date <= ?
                  AND available_at <= ?
                  AND is_latest_revision
            ),
            ranked AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY ratio_code, basis
                        ORDER BY period_end DESC, available_at DESC, ratio_id DESC
                    ) AS rn
                FROM visible
            )
            SELECT
                ratio_code, ratio_category, ratio_kind, basis, unit, value,
                numerator_code, numerator_value, denominator_code, denominator_value,
                period_start, period_end, fiscal_year, fiscal_period,
                as_of_date, available_at, source, upstream_source
            FROM ranked
            WHERE rn = 1
            ORDER BY ratio_category, ratio_code, basis
            """,
            [symbol, latest_date, as_of_ts],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "fundamental_xbrl_metric",
            "xbrl canonical metrics latest per metric",
            """
            WITH visible AS (
                SELECT *
                FROM fundamental_xbrl_metric
                WHERE upper(symbol) = upper(?)
                  AND as_of_date <= ?
                  AND available_at <= ?
                  AND is_latest_revision
            ),
            ranked AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY canonical_metric
                        ORDER BY period_end DESC, available_at DESC, revision_seq DESC, metric_id DESC
                    ) AS rn
                FROM visible
            )
            SELECT
                canonical_metric, concept, taxonomy, unit, period_type, value,
                period_start, period_end, fiscal_year, fiscal_period,
                accession_number, revision_seq, as_of_date, available_at, source
            FROM ranked
            WHERE rn = 1
            ORDER BY canonical_metric
            """,
            [symbol, latest_date, as_of_ts],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "fundamental_statement_points",
            "statement points latest per canonical metric",
            """
            WITH visible AS (
                SELECT *
                FROM fundamental_statement_points
                WHERE upper(symbol) = upper(?)
                  AND as_of_date <= ?
                  AND available_at <= ?
                  AND is_latest_revision
            ),
            ranked AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY statement_type, canonical_metric
                        ORDER BY period_end DESC, available_at DESC, revision_sequence DESC, statement_point_id DESC
                    ) AS rn
                FROM visible
            )
            SELECT
                statement_type, statement_section, canonical_metric, canonical_label,
                value, raw_value, unit, period_type, period_start, period_end,
                fiscal_year, fiscal_period, form, accession_number,
                revision_sequence, revision_count, as_of_date, available_at
            FROM ranked
            WHERE rn = 1
            ORDER BY statement_type, statement_section, canonical_metric
            """,
            [symbol, latest_date, as_of_ts],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "fundamental_ttm_points",
            "ttm points latest per canonical metric",
            """
            WITH visible AS (
                SELECT *
                FROM fundamental_ttm_points
                WHERE upper(symbol) = upper(?)
                  AND as_of_date <= ?
                  AND available_at <= ?
                  AND is_latest_revision
            ),
            ranked AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY statement_type, canonical_metric
                        ORDER BY ttm_end_date DESC, available_at DESC, revision_sequence DESC, ttm_point_id DESC
                    ) AS rn
                FROM visible
            )
            SELECT
                statement_type, statement_section, canonical_metric, canonical_label,
                ttm_value, previous_ttm_value, ttm_value_delta, ttm_value_delta_percent,
                unit, ttm_start_date, ttm_end_date, quarter_count, coverage_days,
                fiscal_year, fiscal_period, form, accession_number, as_of_date, available_at
            FROM ranked
            WHERE rn = 1
            ORDER BY statement_type, statement_section, canonical_metric
            """,
            [symbol, latest_date, as_of_ts],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "fundamental_periods",
            "fundamental periods latest",
            """
            SELECT
                normalized_period_type, period_start, period_end, period_days,
                calendar_period, form, accession_number, statement_point_count,
                canonical_metric_count, concept_count, has_balance_sheet,
                has_income_statement, has_cash_flow, has_per_share,
                revision_sequence, revision_count, as_of_date, available_at
            FROM fundamental_periods
            WHERE upper(symbol) = upper(?)
              AND as_of_date <= ?
              AND available_at <= ?
              AND is_latest_revision
            ORDER BY period_end DESC, available_at DESC
            """,
            [symbol, latest_date, as_of_ts],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "fundamental_growth",
            "fundamental growth latest per formula",
            """
            WITH visible AS (
                SELECT *
                FROM fundamental_growth
                WHERE upper(symbol) = upper(?)
                  AND as_of_date <= ?
                  AND available_at <= ?
                  AND is_latest_revision
            ),
            ranked AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY formula_code, basis, horizon_years
                        ORDER BY period_end DESC, available_at DESC, growth_id DESC
                    ) AS rn
                FROM visible
            )
            SELECT
                formula_code, family, kind, unit, basis, growth_method, horizon_years,
                elapsed_years, value, current_code, current_value, base_code, base_value,
                period_start, period_end, base_period_start, base_period_end,
                fiscal_year, fiscal_period, is_meaningful, vintage_class, as_of_date, available_at
            FROM ranked
            WHERE rn = 1
            ORDER BY family, formula_code, basis, horizon_years
            """,
            [symbol, latest_date, as_of_ts],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        _maybe_section(
            con,
            objects,
            "valuation_multiples",
            "valuation multiples latest per formula",
            """
            WITH visible AS (
                SELECT *
                FROM valuation_multiples
                WHERE upper(symbol) = upper(?)
                  AND trade_date <= ?
                  AND as_of_date <= ?
                  AND available_at <= ?
                  AND is_latest_revision
            ),
            ranked AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY formula_code
                        ORDER BY trade_date DESC, available_at DESC, valuation_multiple_id DESC
                    ) AS rn
                FROM visible
            )
            SELECT
                trade_date, formula_code, category, kind, unit, value,
                numerator_code, numerator_value, denominator_code, denominator_value,
                price, market_cap, enterprise_value, period_end,
                is_meaningful, as_of_date, available_at, source, market_cap_source
            FROM ranked
            WHERE rn = 1
            ORDER BY category, formula_code
            """,
            [symbol, latest_date, latest_date, as_of_ts],
            limit=args.limit,
            max_col_width=args.max_col_width,
        )

        if security_ids:
            placeholders = ", ".join("?" for _ in security_ids)
            factor_params = [*security_ids, latest_date, as_of_ts]
            _maybe_section(
                con,
                objects,
                "fundamental_factor_values",
                "fundamental factor values latest",
                f"""
                SELECT *
                FROM fundamental_factor_values
                WHERE security_id IN ({placeholders})
                  AND as_of_date <= ?
                  AND available_at <= ?
                  AND is_latest_revision
                ORDER BY family, factor_id, as_of_date DESC, available_at DESC
                """,
                factor_params,
                limit=args.limit,
                max_col_width=args.max_col_width,
            )
            _maybe_section(
                con,
                objects,
                "cross_domain_factor_values",
                "cross-domain factor values latest",
                f"""
                SELECT *
                FROM cross_domain_factor_values
                WHERE security_id IN ({placeholders})
                  AND as_of_date <= ?
                  AND available_at <= ?
                  AND is_latest_revision
                ORDER BY domain, family, factor_id, as_of_date DESC, available_at DESC
                """,
                factor_params,
                limit=args.limit,
                max_col_width=args.max_col_width,
            )
            _maybe_section(
                con,
                objects,
                "v_factor_panel",
                "factor panel latest decision date",
                f"""
                SELECT *
                FROM v_factor_panel
                WHERE security_id IN ({placeholders})
                  AND as_of_date = ?
                  AND available_at <= ?
                ORDER BY factor_id
                """,
                factor_params,
                limit=args.limit,
                max_col_width=args.max_col_width,
            )
            _maybe_section(
                con,
                objects,
                "v_factor_panel_wide",
                "factor panel wide latest decision date",
                f"""
                SELECT *
                FROM v_factor_panel_wide
                WHERE security_id IN ({placeholders})
                  AND as_of_date = ?
                ORDER BY security_id
                """,
                [*security_ids, latest_date],
                limit=args.limit,
                max_col_width=args.max_col_width,
            )

        for object_name, title, order_col in (
            ("short_interest_metrics", "short interest metrics latest", "settlement_date"),
            ("short_volume_metrics", "short volume metrics latest", "trade_date"),
            ("thirteenf_concentration_metrics", "13f concentration metrics latest", "report_period"),
            ("thirteenf_option_metrics", "13f option metrics latest", "report_period"),
            ("thirteenf_position_metrics", "13f position metrics latest", "report_period"),
            ("corporate_action_dividend_metrics", "corporate action dividend metrics latest", "ex_date"),
            ("corporate_action_split_metrics", "corporate action split metrics latest", "ex_date"),
            ("corporate_action_factor_reconciliation", "corporate action factor reconciliation latest", "ex_date"),
            ("insider_transaction_metrics", "insider transaction metrics latest", "signal_date"),
            ("security_listing_metrics", "security listing metrics latest", "as_of_date"),
            ("offexchange_volume", "off-exchange volume latest", "summary_end_date"),
            ("offexchange_security_period", "off-exchange security period latest", "summary_end_date"),
        ):
            info = objects.get(object_name)
            if info is None:
                _print_missing(title, object_name)
                continue
            predicate, params = _where_symbol_or_security(info, symbol, security_ids)
            latest_revision_clause = "AND is_latest_revision" if "is_latest_revision" in info.columns else ""
            latest_revision_clause = "AND is_latest" if "is_latest" in info.columns else latest_revision_clause
            asof_clause = ""
            if "as_of_date" in info.columns and "available_at" in info.columns:
                asof_clause = "AND as_of_date <= ? AND available_at <= ?"
                params = [*params, latest_date, as_of_ts]
            _print_section(
                con,
                title,
                f"""
                SELECT *
                FROM {_safe_ident(object_name)}
                WHERE {predicate}
                  {asof_clause}
                  {latest_revision_clause}
                ORDER BY {_safe_ident(order_col)} DESC
                """,
                params,
                limit=args.limit,
                max_col_width=args.max_col_width,
            )

    finally:
        con.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
