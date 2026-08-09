"""Production PIT gross-profitability and quality/value factor definitions."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pit_fundamental_signal_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    rows = (
        (
            "profitability_gross_profitability",
            "PIT annual gross profitability",
            "fundamental_profitability",
            "Annual gross profit divided by same-filing total assets, published by the monthly rebalance close.",
            "method:pit_annual_divide|numerator:gross_profit|denominator:total_assets|annual_days:330-380",
            '["metric:gross_profit","metric:total_assets"]',
        ),
        (
            "value_book_to_market",
            "PIT book-to-market value",
            "fundamental_value",
            "Positive book equity divided by market capitalization from the PIT-visible share count and rebalance close.",
            "method:pit_divide|numerator:stockholders_equity|denominator:market_cap",
            '["metric:stockholders_equity","metric:market_cap"]',
        ),
        (
            "quality_value_gross_profitability",
            "Gross-profitability quality/value composite",
            "fundamental_quality_value",
            "Equal-weight cross-sectional rank of PIT annual gross profitability and book-to-market.",
            "method:mean_percent_rank|left:profitability_gross_profitability|right:value_book_to_market",
            '["factor:profitability_gross_profitability","factor:value_book_to_market"]',
        ),
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id, factor_name, family, description, expression,
            input_ids_json, direction, lookback_days,
            neutralization_spec_json, unit, sign, scale,
            is_point_in_time_safe, available_at_policy, declared_in,
            owner, source, standardization_spec_json, valid_from, valid_to
        ) VALUES (
            ?, ?, ?, ?, ?, ?, 1, 0,
            '{"method":"none","by":[]}', 'ratio', 'signed', 'zscore',
            true,
            'Available after the month-end close when the price, share count, and same-filing annual fundamentals are all visible.',
            'atx_db.fundamental_signals', 'atx-db',
            'atx-db PIT fundamental signals v1',
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01]}',
            DATE '1900-01-01', NULL
        )
        """,
        rows,
    )

    factor_ids = tuple(row[0] for row in rows)
    placeholders = ", ".join("?" for _ in factor_ids)
    conn.execute(
        f"DELETE FROM factor_dependency_edges WHERE factor_id IN ({placeholders})",
        list(factor_ids),
    )
    dependencies = (
        ("profitability_gross_profitability", "metric", "gross_profit", None, "gross_profit"),
        ("profitability_gross_profitability", "metric", "total_assets", None, "total_assets"),
        ("value_book_to_market", "metric", "stockholders_equity", None, "stockholders_equity"),
        ("value_book_to_market", "metric", "market_cap", None, "market_cap"),
        (
            "quality_value_gross_profitability",
            "factor",
            "profitability_gross_profitability",
            "profitability_gross_profitability",
            None,
        ),
        (
            "quality_value_gross_profitability",
            "factor",
            "value_book_to_market",
            "value_book_to_market",
            None,
        ),
    )
    conn.executemany(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id, factor_id, dependency_type, dependency_name,
            dependency_factor_id, dependency_metric_id, dependency_source_id,
            dependency_depth, expression, lookback_days, is_direct, source
        ) VALUES (
            sha256(concat_ws('|', 'pit_fundamental_signal', ?, ?, ?)),
            ?, ?, ?, ?, ?, NULL, 1, ?, 0, true,
            'atx-db PIT fundamental signals v1'
        )
        """,
        [
            (
                factor_id,
                dependency_type,
                dependency_name,
                factor_id,
                dependency_type,
                dependency_name,
                dependency_factor_id,
                dependency_metric_id,
                f"{dependency_type}:{dependency_name}",
            )
            for (
                factor_id,
                dependency_type,
                dependency_name,
                dependency_factor_id,
                dependency_metric_id,
            ) in dependencies
        ],
    )
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_factor_family_catalog AS
        SELECT
            fd.factor_id,
            fd.factor_name,
            fd.family,
            fd.description,
            fd.expression,
            fd.input_ids_json,
            fd.direction,
            fd.standardization_spec_json,
            fd.neutralization_spec_json,
            fd.unit,
            fd.sign,
            fd.scale,
            fd.valid_from,
            fd.valid_to,
            coalesce(edges.dependency_count, 0) AS dependency_count,
            fd.source
        FROM factor_definition fd
        LEFT JOIN (
            SELECT factor_id, count(*)::BIGINT AS dependency_count
            FROM factor_dependency_edges
            GROUP BY factor_id
        ) edges ON edges.factor_id = fd.factor_id
        WHERE fd.declared_in IN (
            'db/seeds/factor_definitions.csv',
            'atx_db.fundamental_signals'
        )
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=194, name="pit_fundamental_signal_catalog", up=_pit_fundamental_signal_catalog)
]
