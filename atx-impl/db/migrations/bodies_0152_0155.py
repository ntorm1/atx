"""PF3-S7 migration bodies: governed factor framework surfaces."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pf3_s7_factor_definition_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S7 S7-0: definition-as-data factor catalog seeded from legacy surfaces."""

    from ..factors.catalog import legacy_factor_definitions

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_definition (
            factor_id VARCHAR PRIMARY KEY,
            factor_name VARCHAR NOT NULL,
            family VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            expression VARCHAR NOT NULL,
            input_ids_json VARCHAR NOT NULL,
            direction INTEGER NOT NULL,
            lookback_days INTEGER NOT NULL,
            neutralization_spec_json VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            sign VARCHAR NOT NULL,
            scale VARCHAR NOT NULL,
            is_point_in_time_safe BOOLEAN NOT NULL DEFAULT true,
            available_at_policy VARCHAR NOT NULL,
            declared_in VARCHAR NOT NULL,
            owner VARCHAR,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    rows = legacy_factor_definitions()
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
            source
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            (
                row.factor_id,
                row.factor_name,
                row.family,
                row.description,
                row.expression,
                row.input_ids_json,
                row.direction,
                row.lookback_days,
                row.neutralization_spec_json,
                row.unit,
                row.sign,
                row.scale,
                row.is_point_in_time_safe,
                row.available_at_policy,
                row.declared_in,
                row.owner,
                row.source,
            )
            for row in rows
        ],
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'factor_definition',
            'atx_warehouse',
            'Governed factor definition catalog',
            'Definition-as-data factor catalog reconciling legacy feature definitions and alpha expressions into one governed factor namespace.',
            'factor_id',
            'factor_definition',
            'source_loaded_at',
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
            'factor_definition',
            'gold',
            'factor_definition',
            'factor_id',
            'Governed definition-as-data factor catalog. S7 seeds legacy feature_definitions, fundamental feature definitions, and alpha expression specs as the first governed factor rows.',
            '["factor_id"]',
            'Catalog metadata is knowledge-time data. Compute-time factors must still gate inputs with available_at <= as-of and preserve source_loaded_at/run_id on value rows.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("factor_definition",))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('factor_definition', 'factor_id', 'identifier', 'Stable factor identifier used by the factor DAG and downstream panels.', false, NULL, NULL, now()),
            ('factor_definition', 'factor_name', 'text', 'Human-readable factor name.', false, NULL, NULL, now()),
            ('factor_definition', 'family', 'category', 'Factor family such as returns, momentum, liquidity, fundamental_value, or alpha_research.', false, NULL, NULL, now()),
            ('factor_definition', 'description', 'text', 'Human-readable factor definition.', false, NULL, NULL, now()),
            ('factor_definition', 'expression', 'text', 'Declarative expression inherited from feature_definitions or alpha_expression_catalog.', false, NULL, NULL, now()),
            ('factor_definition', 'input_ids_json', 'json', 'JSON list of typed factor, metric, ratio, or source dependencies.', false, NULL, NULL, now()),
            ('factor_definition', 'direction', 'measure', 'Signal direction: 1 when higher is better, -1 when lower is better.', false, 'direction', NULL, now()),
            ('factor_definition', 'lookback_days', 'duration', 'Declared feature/factor lookback in calendar days or source rows, inherited from legacy definitions.', false, 'days', NULL, now()),
            ('factor_definition', 'neutralization_spec_json', 'json', 'JSON neutralization declaration. S7-0 seeds none; S7-3 adds executable neutralization.', false, NULL, NULL, now()),
            ('factor_definition', 'unit', 'category', 'Semantic unit for values emitted by this factor.', false, 'unit', NULL, now()),
            ('factor_definition', 'sign', 'category', 'Semantic sign convention for values emitted by this factor.', false, 'sign', NULL, now()),
            ('factor_definition', 'scale', 'category', 'Semantic scale convention for values emitted by this factor.', false, 'scale', NULL, now()),
            ('factor_definition', 'is_point_in_time_safe', 'flag', 'True when the factor is declared PIT-safe and eligible for S7 engine materialization.', false, 'boolean', NULL, now()),
            ('factor_definition', 'available_at_policy', 'text', 'Policy describing when the factor becomes visible to as-of readers.', false, NULL, NULL, now()),
            ('factor_definition', 'declared_in', 'category', 'Legacy surface that declared the factor row.', false, NULL, NULL, now()),
            ('factor_definition', 'owner', 'identifier', 'Owning team or module.', true, NULL, NULL, now()),
            ('factor_definition', 'source', 'identifier', 'Source module that produced the catalog row.', false, NULL, NULL, now()),
            ('factor_definition', 'source_loaded_at', 'timestamp', 'Warehouse timestamp when the catalog row was loaded.', false, 'timestamp', NULL, now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=152,
        name="pf3_s7_factor_definition_catalog",
        up=_pf3_s7_factor_definition_catalog,
    ),
]
