"""PF3-S8 migration bodies: fundamental factor families."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pf3_s8_core_academic_factor_families(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S8 S8-0: seed value, quality, and profitability factor families."""

    for statement in (
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS standardization_spec_json VARCHAR",
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS valid_from DATE",
        "ALTER TABLE factor_definition ADD COLUMN IF NOT EXISTS valid_to DATE",
    ):
        conn.execute(statement)

    from ..factors.engine import factor_dependency_edges_frame
    from ..factors.fundamental_families import factor_seed_definitions, factor_seed_frame

    seed = factor_seed_frame(stages=("s8_0",))
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
    edges = factor_dependency_edges_frame(factor_seed_definitions(stages=("s8_0",)))
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
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_factor_families',
            'atx_warehouse',
            'Fundamental factor families',
            'Definition-as-data academic fundamental factor families seeded into factor_definition and computed through the S7 factor framework.',
            'factor_id',
            'factor_definition',
            'valid_from',
            'source_loaded_at',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("factor_definition", "factor_dependency_edges"))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('factor_definition', 'standardization_spec_json', 'json', 'JSON standardization/operator directive for this factor definition, such as zscore_cs.', true, NULL, 'db/seeds/factor_definitions.csv.standardization', now()),
            ('factor_definition', 'valid_from', 'date', 'Definition valid-from date from the factor definition seed.', true, 'date', 'db/seeds/factor_definitions.csv.valid_from', now()),
            ('factor_definition', 'valid_to', 'date', 'Definition valid-to date from the factor definition seed; NULL means open-ended.', true, 'date', 'db/seeds/factor_definitions.csv.valid_to', now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


def _upsert_factor_seed_stage(conn: duckdb.DuckDBPyConnection, stage: str) -> None:
    from ..factors.engine import factor_dependency_edges_frame
    from ..factors.fundamental_families import factor_seed_definitions, factor_seed_frame

    seed = factor_seed_frame(stages=(stage,))
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
    edges = factor_dependency_edges_frame(factor_seed_definitions(stages=(stage,)))
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


def _pf3_s8_growth_investment_leverage_composites(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S8 S8-1: growth, investment, leverage, and named composite definitions."""

    _upsert_factor_seed_stage(conn, "s8_1")
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_composite_factors',
            'atx_warehouse',
            'Named fundamental composite factors',
            'Piotroski F-score, Altman Z-score, Sloan accruals, plus growth/investment/leverage definition rows seeded into factor_definition.',
            'factor_id',
            'factor_definition',
            'valid_from',
            'source_loaded_at',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("factor_definition", "factor_dependency_edges"))
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s8_signal_native_factor_definitions(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S8 S8-2: signal-native factor definitions for surpass axis 2."""

    _upsert_factor_seed_stage(conn, "s8_2")
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'signal_native_factors',
            'atx_warehouse',
            'Signal-native fundamental factors',
            'Warehouse-native PIT revisions momentum, standardization-delta, segment concentration, and footnote disclosure-change factor definitions.',
            'factor_id',
            'factor_definition',
            'valid_from',
            'source_loaded_at',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("factor_definition", "factor_dependency_edges"))
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s8_family_panel_catalog_and_gates(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S8 S8-3: factor-family panel surface, catalog view, and gate registration."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_factor_values (
            factor_value_id VARCHAR PRIMARY KEY,
            factor_id VARCHAR NOT NULL,
            factor_name VARCHAR NOT NULL,
            family VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            as_of_date DATE NOT NULL,
            raw_value DOUBLE,
            value DOUBLE,
            available_at TIMESTAMP NOT NULL,
            input_ids_json VARCHAR NOT NULL,
            input_lineage_json VARCHAR NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_factor_values_factor_asof ON fundamental_factor_values(factor_id, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_factor_values_family_asof ON fundamental_factor_values(family, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_factor_values_security_asof ON fundamental_factor_values(security_id, as_of_date)",
    ):
        conn.execute(statement)
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
        ) edges
          ON edges.factor_id = fd.factor_id
        WHERE fd.declared_in = 'db/seeds/factor_definitions.csv'
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_factor_values',
            'atx_warehouse',
            'Fundamental factor family values',
            'PIT factor-family panel rows emitted by PF3-S8 academic and signal-native factor computations.',
            'factor_id,security_id,as_of_date',
            'fundamental_factor_values',
            'as_of_date',
            'available_at',
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
        VALUES
            (
                'fundamental_factor_values',
                'gold',
                'fundamental_factor_value',
                'factor_id,security_id,as_of_date',
                'PIT fundamental factor family panel values with raw and standardized values plus full input lineage.',
                '["factor_value_id"]',
                'Use available_at <= decision timestamp and as_of_date <= decision date. raw_value is the family formula output; value is standardized through S7 operators.',
                now()
            ),
            (
                'v_fundamental_factor_family_catalog',
                'view',
                'fundamental_factor_family_catalog',
                'factor_id',
                'Queryable catalog of PF3-S8 factor-family definitions, semantic metadata, and dependency counts.',
                '["factor_id"]',
                'Definition metadata is knowledge-time data; factor values remain PIT-gated by fundamental_factor_values.available_at.',
                now()
            )
        """
    )
    _catalog_fields_for_tables(conn, ("fundamental_factor_values", "v_fundamental_factor_family_catalog"))
    conn.execute(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name, dataset_id, table_name, severity, threshold_value,
            comparator, enabled, failure_status, source, updated_at
        )
        VALUES
            (
                'fundamental_factor_lineage_completeness',
                'fundamental_factor_values',
                'fundamental_factor_values',
                'critical',
                0.0,
                'eq',
                true,
                'failed',
                'pf3_s8',
                now()
            ),
            (
                'fundamental_factor_family_coverage',
                'fundamental_factor_values',
                'fundamental_factor_values',
                'critical',
                0.0,
                'eq',
                true,
                'failed',
                'pf3_s8',
                now()
            )
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=156,
        name="pf3_s8_core_academic_factor_families",
        up=_pf3_s8_core_academic_factor_families,
    ),
    Migration(
        version=157,
        name="pf3_s8_growth_investment_leverage_composites",
        up=_pf3_s8_growth_investment_leverage_composites,
    ),
    Migration(
        version=158,
        name="pf3_s8_signal_native_factor_definitions",
        up=_pf3_s8_signal_native_factor_definitions,
    ),
    Migration(
        version=159,
        name="pf3_s8_family_panel_catalog_and_gates",
        up=_pf3_s8_family_panel_catalog_and_gates,
    ),
]
