"""PF3-S9 migration bodies: cross-domain factor namespace."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pf3_s9_price_liquidity_factor_definitions(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S9 S9-0: seed price/liquidity factors into the S7 namespace."""

    for statement in (
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS standardization_spec_json VARCHAR",
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS valid_from DATE",
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS valid_to DATE",
    ):
        conn.execute(statement)

    from ..factors.cross_domain import price_liquidity_definition_frame, price_liquidity_dependency_edges_frame

    seed = price_liquidity_definition_frame()
    conn.executemany(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,
            factor_name,
            family,
            description,
            expression,
            input_ids_json,
            direction,
            lookback_days,
            neutralization_spec_json,
            unit,
            sign,
            scale,
            is_point_in_time_safe,
            available_at_policy,
            declared_in,
            owner,
            source,
            standardization_spec_json,
            valid_from,
            valid_to
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(? AS DATE), CAST(? AS DATE))
        """,
        [
            (
                row.factor_id,
                row.factor_name,
                row.family,
                row.description,
                row.expression,
                row.input_ids_json,
                int(row.direction),
                int(row.lookback_days),
                row.neutralization_spec_json,
                row.unit,
                row.sign,
                row.scale,
                bool(row.is_point_in_time_safe),
                row.available_at_policy,
                row.declared_in,
                row.owner,
                row.source,
                row.standardization_spec_json,
                row.valid_from,
                row.valid_to,
            )
            for row in seed.itertuples(index=False)
        ],
    )

    edges = price_liquidity_dependency_edges_frame()
    if not edges.empty:
        conn.executemany(
            """
            INSERT OR REPLACE INTO factor_dependency_edges (
                dependency_id,
                factor_id,
                dependency_type,
                dependency_name,
                dependency_factor_id,
                dependency_metric_id,
                dependency_source_id,
                dependency_depth,
                expression,
                lookback_days,
                is_direct,
                source
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            list(
                edges[
                    [
                        "dependency_id",
                        "factor_id",
                        "dependency_type",
                        "dependency_name",
                        "dependency_factor_id",
                        "dependency_metric_id",
                        "dependency_source_id",
                        "dependency_depth",
                        "expression",
                        "lookback_days",
                        "is_direct",
                        "source",
                    ]
                ].itertuples(index=False, name=None)
            ),
        )

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_cross_domain_price_liquidity_factor_catalog AS
        SELECT
            fd.factor_id,
            fd.factor_name,
            'price_liquidity' AS domain,
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
        ) edges
          ON edges.factor_id = fd.factor_id
        WHERE fd.declared_in = 'db.factors.cross_domain.price_liquidity_specs'
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'cross_domain_price_liquidity_factors',
            'atx_warehouse',
            'Cross-domain price/liquidity factors',
            'Price, liquidity, and market-risk analytics from equity_price_metrics registered as S7 factor_definition rows.',
            'factor_id',
            'factor_definition',
            'valid_from',
            'source_loaded_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'v_cross_domain_price_liquidity_factor_catalog',
            'view',
            'cross_domain_factor_catalog',
            'factor_id',
            'Queryable S9 price/liquidity factor catalog with semantic metadata and dependency counts.',
            '["factor_id"]',
            'Definition metadata is knowledge-time data; emitted factor values inherit available_at from equity_price_metrics.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("factor_definition", "factor_dependency_edges", "v_cross_domain_price_liquidity_factor_catalog"))
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s9_estimate_13f_factor_definitions(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S9 S9-1: seed estimate-revision and 13F-flow factors."""

    for statement in (
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS standardization_spec_json VARCHAR",
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS valid_from DATE",
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS valid_to DATE",
    ):
        conn.execute(statement)

    from ..factors.cross_domain import estimate_13f_definition_frame, estimate_13f_dependency_edges_frame

    seed = estimate_13f_definition_frame()
    conn.executemany(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,
            factor_name,
            family,
            description,
            expression,
            input_ids_json,
            direction,
            lookback_days,
            neutralization_spec_json,
            unit,
            sign,
            scale,
            is_point_in_time_safe,
            available_at_policy,
            declared_in,
            owner,
            source,
            standardization_spec_json,
            valid_from,
            valid_to
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(? AS DATE), CAST(? AS DATE))
        """,
        [
            (
                row.factor_id,
                row.factor_name,
                row.family,
                row.description,
                row.expression,
                row.input_ids_json,
                int(row.direction),
                int(row.lookback_days),
                row.neutralization_spec_json,
                row.unit,
                row.sign,
                row.scale,
                bool(row.is_point_in_time_safe),
                row.available_at_policy,
                row.declared_in,
                row.owner,
                row.source,
                row.standardization_spec_json,
                row.valid_from,
                row.valid_to,
            )
            for row in seed.itertuples(index=False)
        ],
    )

    edges = estimate_13f_dependency_edges_frame()
    if not edges.empty:
        conn.executemany(
            """
            INSERT OR REPLACE INTO factor_dependency_edges (
                dependency_id,
                factor_id,
                dependency_type,
                dependency_name,
                dependency_factor_id,
                dependency_metric_id,
                dependency_source_id,
                dependency_depth,
                expression,
                lookback_days,
                is_direct,
                source
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            list(
                edges[
                    [
                        "dependency_id",
                        "factor_id",
                        "dependency_type",
                        "dependency_name",
                        "dependency_factor_id",
                        "dependency_metric_id",
                        "dependency_source_id",
                        "dependency_depth",
                        "expression",
                        "lookback_days",
                        "is_direct",
                        "source",
                    ]
                ].itertuples(index=False, name=None)
            ),
        )

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_cross_domain_estimate_13f_factor_catalog AS
        SELECT
            fd.factor_id,
            fd.factor_name,
            CASE
                WHEN fd.factor_id LIKE 'estimate_%' THEN 'estimate_revision'
                ELSE '13f_flow'
            END AS domain,
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
        ) edges
          ON edges.factor_id = fd.factor_id
        WHERE fd.declared_in = 'db.factors.cross_domain.estimate_13f_specs'
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'cross_domain_estimate_13f_factors',
            'atx_warehouse',
            'Cross-domain estimate and 13F factors',
            'Estimate-surprise, consensus-revision, and 13F concentration/flow analytics registered as S7 factor_definition rows.',
            'factor_id',
            'factor_definition',
            'valid_from',
            'source_loaded_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'v_cross_domain_estimate_13f_factor_catalog',
            'view',
            'cross_domain_factor_catalog',
            'factor_id',
            'Queryable S9 estimate-revision and 13F-flow factor catalog with semantic metadata and dependency counts.',
            '["factor_id"]',
            'Definition metadata is knowledge-time data; emitted values inherit available_at from est_surprise, est_consensus, and thirteenf_concentration_metrics.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("factor_definition", "factor_dependency_edges", "v_cross_domain_estimate_13f_factor_catalog"))
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s9_short_insider_factor_definitions(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S9 S9-2: seed short-interest and insider factors."""

    for statement in (
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS standardization_spec_json VARCHAR",
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS valid_from DATE",
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS valid_to DATE",
    ):
        conn.execute(statement)

    from ..factors.cross_domain import short_insider_definition_frame, short_insider_dependency_edges_frame

    seed = short_insider_definition_frame()
    conn.executemany(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,
            factor_name,
            family,
            description,
            expression,
            input_ids_json,
            direction,
            lookback_days,
            neutralization_spec_json,
            unit,
            sign,
            scale,
            is_point_in_time_safe,
            available_at_policy,
            declared_in,
            owner,
            source,
            standardization_spec_json,
            valid_from,
            valid_to
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(? AS DATE), CAST(? AS DATE))
        """,
        [
            (
                row.factor_id,
                row.factor_name,
                row.family,
                row.description,
                row.expression,
                row.input_ids_json,
                int(row.direction),
                int(row.lookback_days),
                row.neutralization_spec_json,
                row.unit,
                row.sign,
                row.scale,
                bool(row.is_point_in_time_safe),
                row.available_at_policy,
                row.declared_in,
                row.owner,
                row.source,
                row.standardization_spec_json,
                row.valid_from,
                row.valid_to,
            )
            for row in seed.itertuples(index=False)
        ],
    )

    edges = short_insider_dependency_edges_frame()
    if not edges.empty:
        conn.executemany(
            """
            INSERT OR REPLACE INTO factor_dependency_edges (
                dependency_id,
                factor_id,
                dependency_type,
                dependency_name,
                dependency_factor_id,
                dependency_metric_id,
                dependency_source_id,
                dependency_depth,
                expression,
                lookback_days,
                is_direct,
                source
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            list(
                edges[
                    [
                        "dependency_id",
                        "factor_id",
                        "dependency_type",
                        "dependency_name",
                        "dependency_factor_id",
                        "dependency_metric_id",
                        "dependency_source_id",
                        "dependency_depth",
                        "expression",
                        "lookback_days",
                        "is_direct",
                        "source",
                    ]
                ].itertuples(index=False, name=None)
            ),
        )

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_cross_domain_short_insider_factor_catalog AS
        SELECT
            fd.factor_id,
            fd.factor_name,
            CASE
                WHEN fd.factor_id LIKE 'short_interest_%' THEN 'short_interest'
                ELSE 'insider'
            END AS domain,
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
        ) edges
          ON edges.factor_id = fd.factor_id
        WHERE fd.declared_in = 'db.factors.cross_domain.short_insider_specs'
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'cross_domain_short_insider_factors',
            'atx_warehouse',
            'Cross-domain short-interest and insider factors',
            'Short-interest crowding/flow and insider net-buy/cluster/plan-sale analytics registered as S7 factor_definition rows.',
            'factor_id',
            'factor_definition',
            'valid_from',
            'source_loaded_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'v_cross_domain_short_insider_factor_catalog',
            'view',
            'cross_domain_factor_catalog',
            'factor_id',
            'Queryable S9 short-interest and insider factor catalog with semantic metadata and dependency counts.',
            '["factor_id"]',
            'Definition metadata is knowledge-time data; emitted values inherit available_at from short_interest_metrics and insider_transaction_metrics.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("factor_definition", "factor_dependency_edges", "v_cross_domain_short_insider_factor_catalog"))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=160,
        name="pf3_s9_price_liquidity_factor_definitions",
        up=_pf3_s9_price_liquidity_factor_definitions,
    ),
    Migration(
        version=161,
        name="pf3_s9_estimate_13f_factor_definitions",
        up=_pf3_s9_estimate_13f_factor_definitions,
    ),
    Migration(
        version=162,
        name="pf3_s9_short_insider_factor_definitions",
        up=_pf3_s9_short_insider_factor_definitions,
    ),
]
