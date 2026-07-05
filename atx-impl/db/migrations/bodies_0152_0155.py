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


def _pf3_s7_factor_dependency_engine_tables(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S7 S7-1: factor dependency DAG and build-manifest surfaces."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_dependency_edges (
            dependency_id VARCHAR PRIMARY KEY,
            factor_id VARCHAR NOT NULL,
            dependency_type VARCHAR NOT NULL,
            dependency_name VARCHAR NOT NULL,
            dependency_factor_id VARCHAR,
            dependency_metric_id VARCHAR,
            dependency_source_id VARCHAR,
            dependency_depth INTEGER NOT NULL,
            expression VARCHAR,
            lookback_days INTEGER,
            is_direct BOOLEAN NOT NULL DEFAULT true,
            source VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_build_manifests (
            manifest_id VARCHAR PRIMARY KEY,
            run_id VARCHAR,
            factor_ids_json VARCHAR NOT NULL,
            topological_order_json VARCHAR NOT NULL,
            input_row_count BIGINT NOT NULL,
            output_row_count BIGINT NOT NULL,
            output_min_as_of_date DATE,
            output_max_as_of_date DATE,
            min_available_at TIMESTAMP,
            max_available_at TIMESTAMP,
            params_json VARCHAR,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    from ..factors.engine import legacy_factor_dependency_edges_frame

    edges = legacy_factor_dependency_edges_frame()
    if not edges.empty:
        conn.execute("DELETE FROM factor_dependency_edges")
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
        VALUES
            (
                'factor_dependency_edges',
                'atx_warehouse',
                'Factor dependency DAG edges',
                'Typed dependency graph linking governed factor definitions to factor, metric, ratio, and source inputs.',
                'factor_id,dependency_type,dependency_name',
                'factor_dependency_edges',
                'updated_at',
                'updated_at',
                now()
            ),
            (
                'factor_build_manifests',
                'atx_warehouse',
                'Factor engine build manifests',
                'Per-run factor engine manifest recording target factors, topological order, input/output counts, and availability range.',
                'manifest_id',
                'factor_build_manifests',
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
        VALUES
            (
                'factor_dependency_edges',
                'gold',
                'factor_dependency',
                'factor_id,dependency_type,dependency_name',
                'Typed factor dependency graph extending the legacy feature_dependency_edges concept into the governed factor namespace.',
                '["dependency_id"]',
                'Dependency metadata is knowledge-time data. Engine materialization must still enforce available_at <= as-of on value inputs.',
                now()
            ),
            (
                'factor_build_manifests',
                'gold',
                'factor_build_manifest',
                'manifest_id',
                'Per-run factor engine manifest with dependency order, row counts, and availability window for deterministic rebuild checks.',
                '["manifest_id"]',
                'Manifest availability summarizes the emitted factor rows; downstream PIT readers should use factor value available_at, not manifest timestamps.',
                now()
            )
        """
    )
    _catalog_fields_for_tables(conn, ("factor_dependency_edges", "factor_build_manifests"))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('factor_dependency_edges', 'dependency_id', 'identifier', 'Stable hash identifier for this factor dependency edge.', false, NULL, NULL, now()),
            ('factor_dependency_edges', 'factor_id', 'identifier', 'Governed factor id from factor_definition.', false, NULL, 'factor_definition.factor_id', now()),
            ('factor_dependency_edges', 'dependency_type', 'category', 'Dependency namespace: factor, metric, ratio, or source.', false, NULL, NULL, now()),
            ('factor_dependency_edges', 'dependency_name', 'identifier', 'Dependency identifier inside dependency_type.', false, NULL, NULL, now()),
            ('factor_dependency_edges', 'dependency_factor_id', 'identifier', 'Referenced factor id when dependency_type=factor.', true, NULL, 'factor_definition.factor_id', now()),
            ('factor_dependency_edges', 'dependency_metric_id', 'identifier', 'Referenced metric or ratio id when dependency_type=metric/ratio.', true, NULL, 'v_metric_catalog.metric_code', now()),
            ('factor_dependency_edges', 'dependency_source_id', 'identifier', 'Referenced source table or external source id when dependency_type=source.', true, NULL, NULL, now()),
            ('factor_dependency_edges', 'dependency_depth', 'count', 'Dependency depth in the declared factor DAG. S7-1 seeds direct edges at depth 1.', false, 'count', NULL, now()),
            ('factor_dependency_edges', 'expression', 'text', 'Factor expression associated with the dependency edge.', true, NULL, 'factor_definition.expression', now()),
            ('factor_dependency_edges', 'lookback_days', 'duration', 'Declared lookback inherited from factor_definition.', true, 'days', 'factor_definition.lookback_days', now()),
            ('factor_dependency_edges', 'is_direct', 'flag', 'True when this is a direct declared dependency.', false, 'boolean', NULL, now()),
            ('factor_dependency_edges', 'source', 'identifier', 'Source module that produced the edge row.', false, NULL, NULL, now()),
            ('factor_build_manifests', 'manifest_id', 'identifier', 'Stable manifest id for a factor engine build.', false, NULL, NULL, now()),
            ('factor_build_manifests', 'run_id', 'identifier', 'Optional caller-supplied run id.', true, NULL, NULL, now()),
            ('factor_build_manifests', 'factor_ids_json', 'json', 'JSON list of target factor ids in the build.', false, NULL, NULL, now()),
            ('factor_build_manifests', 'topological_order_json', 'json', 'JSON list of dependency-first factor ids visited by the engine.', false, NULL, NULL, now()),
            ('factor_build_manifests', 'input_row_count', 'count', 'Input factor value rows read by the engine.', false, 'count', NULL, now()),
            ('factor_build_manifests', 'output_row_count', 'count', 'Output factor rows emitted by the engine.', false, 'count', NULL, now()),
            ('factor_build_manifests', 'output_min_as_of_date', 'date', 'Minimum as-of date emitted by the build.', true, 'date', NULL, now()),
            ('factor_build_manifests', 'output_max_as_of_date', 'date', 'Maximum as-of date emitted by the build.', true, 'date', NULL, now()),
            ('factor_build_manifests', 'min_available_at', 'timestamp', 'Minimum factor availability timestamp emitted by the build.', true, 'timestamp', NULL, now()),
            ('factor_build_manifests', 'max_available_at', 'timestamp', 'Maximum factor availability timestamp emitted by the build.', true, 'timestamp', NULL, now()),
            ('factor_build_manifests', 'params_json', 'json', 'Build parameter payload.', true, NULL, NULL, now()),
            ('factor_build_manifests', 'source', 'identifier', 'Source module that produced the manifest.', false, NULL, NULL, now()),
            ('factor_build_manifests', 'source_loaded_at', 'timestamp', 'Warehouse timestamp when the manifest row was loaded.', false, 'timestamp', NULL, now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=152,
        name="pf3_s7_factor_definition_catalog",
        up=_pf3_s7_factor_definition_catalog,
    ),
    Migration(
        version=153,
        name="pf3_s7_factor_dependency_engine_tables",
        up=_pf3_s7_factor_dependency_engine_tables,
    ),
]
