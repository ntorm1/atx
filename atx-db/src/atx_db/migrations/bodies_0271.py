"""Production standardized-fundamentals revision and build contracts."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin
from .bodies_0267 import _seed_public_contract


def _standardized_fundamentals_release(conn: duckdb.DuckDBPyConnection) -> None:
    for statement in (
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS unit VARCHAR",
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS revision_group_id VARCHAR",
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS revision_sequence INTEGER",
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS revision_count INTEGER",
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS is_value_changed BOOLEAN",
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS previous_value DOUBLE",
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS value_delta DOUBLE",
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS value_delta_percent DOUBLE",
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS update_type VARCHAR",
        "ALTER TABLE fundamental_standardized ADD COLUMN IF NOT EXISTS valid_to TIMESTAMP",
    ):
        conn.execute(statement)

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_standardization_coverage AS
        WITH template AS (
            SELECT 'annual' AS basis, 130.0 AS template_item_count
            UNION ALL SELECT 'quarterly', 130.0
            UNION ALL SELECT 'ttm', 130.0
            UNION ALL SELECT 'instant', 60.0
        ),
        std AS (
            SELECT
                source,security_id,any_value(symbol) AS symbol,any_value(cik) AS cik,
                period_end,basis,max(available_at) AS available_at,
                count(DISTINCT item_id)::DOUBLE AS populated_item_count,
                sum(CASE WHEN combination_rule IN ('identity','coalesce_priority') THEN 1 ELSE 0 END)::DOUBLE AS direct_or_coalesce_count,
                sum(CASE WHEN combination_rule = 'sum' THEN 1 ELSE 0 END)::DOUBLE AS sum_count,
                sum(CASE WHEN combination_rule = 'difference' THEN 1 ELSE 0 END)::DOUBLE AS difference_count,
                sum(CASE WHEN combination_rule = 'first_non_null' THEN 1 ELSE 0 END)::DOUBLE AS first_non_null_count
            FROM fundamental_standardized
            WHERE is_latest_revision
            GROUP BY source,security_id,period_end,basis
        ),
        exc AS (
            SELECT
                source,security_id,any_value(symbol) AS symbol,any_value(cik) AS cik,
                period_end,basis,max(available_at) AS available_at,
                count(*)::DOUBLE AS exception_count
            FROM fundamental_standardization_exception
            WHERE is_latest_revision
            GROUP BY source,security_id,period_end,basis
        ),
        keys AS (
            SELECT source,security_id,symbol,cik,period_end,basis FROM std
            UNION
            SELECT source,security_id,symbol,cik,period_end,basis FROM exc
        )
        SELECT
            k.source,k.security_id,coalesce(s.symbol,e.symbol,k.symbol) AS symbol,
            coalesce(s.cik,e.cik,k.cik) AS cik,k.period_end,k.basis,
            greatest(coalesce(s.available_at,TIMESTAMP '1900-01-01'),
                     coalesce(e.available_at,TIMESTAMP '1900-01-01')) AS available_at,
            coalesce(s.populated_item_count,0.0) AS populated_item_count,
            coalesce(t.template_item_count,0.0) AS template_item_count,
            CASE WHEN coalesce(t.template_item_count,0.0) = 0.0 THEN NULL
                 ELSE coalesce(s.populated_item_count,0.0) / t.template_item_count END AS coverage_ratio,
            coalesce(s.direct_or_coalesce_count,0.0) AS direct_or_coalesce_count,
            coalesce(s.sum_count,0.0) AS sum_count,
            coalesce(s.difference_count,0.0) AS difference_count,
            coalesce(s.first_non_null_count,0.0) AS first_non_null_count,
            coalesce(e.exception_count,0.0) AS exception_count
        FROM keys k
        LEFT JOIN std s USING (source,security_id,period_end,basis)
        LEFT JOIN exc e USING (source,security_id,period_end,basis)
        LEFT JOIN template t USING (basis)
        """
    )

    conn.execute(
        """
        CREATE TEMP TABLE _fundamental_standardized_revision_backfill AS
        SELECT
            standardized_id,
            sha256(concat_ws('|', source, security_id, CAST(item_id AS VARCHAR), basis,
                             CAST(period_end AS VARCHAR), rule_id)) AS revision_group_id,
            row_number() OVER revision_window AS revision_sequence,
            count(*) OVER revision_window AS revision_count,
            lag(value) OVER revision_window AS previous_value,
            lead(available_at) OVER revision_window AS valid_to
        FROM fundamental_standardized
        WINDOW revision_window AS (
            PARTITION BY source, security_id, item_id, basis, period_end, rule_id
            ORDER BY available_at, source_loaded_at, standardized_id
            ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
        )
        """
    )
    conn.execute(
        """
        UPDATE fundamental_standardized AS target
        SET
            unit = coalesce(target.unit, target.unit_type),
            revision_group_id = source.revision_group_id,
            revision_sequence = source.revision_sequence,
            revision_count = source.revision_count,
            previous_value = source.previous_value,
            is_value_changed = CASE
                WHEN source.revision_sequence = 1 THEN false
                ELSE target.value IS DISTINCT FROM source.previous_value
            END,
            value_delta = CASE
                WHEN source.previous_value IS NULL THEN NULL
                ELSE target.value - source.previous_value
            END,
            value_delta_percent = CASE
                WHEN source.previous_value IS NULL OR source.previous_value = 0 THEN NULL
                ELSE (target.value - source.previous_value) / abs(source.previous_value)
            END,
            update_type = CASE WHEN source.revision_sequence = 1 THEN 'original' ELSE 'restated' END,
            valid_to = source.valid_to,
            is_latest_revision = source.revision_sequence = source.revision_count
        FROM _fundamental_standardized_revision_backfill AS source
        WHERE target.standardized_id = source.standardized_id
        """
    )
    conn.execute("DROP TABLE _fundamental_standardized_revision_backfill")
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_standardization_builds (
            build_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            run_id VARCHAR NOT NULL,
            rule_set_sha256 VARCHAR NOT NULL,
            rule_count INTEGER NOT NULL,
            scope_json VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            input_row_count BIGINT NOT NULL DEFAULT 0,
            standardized_row_count BIGINT NOT NULL DEFAULT 0,
            exception_row_count BIGINT NOT NULL DEFAULT 0,
            basis_counts_json VARCHAR NOT NULL DEFAULT '{}',
            exception_reason_counts_json VARCHAR NOT NULL DEFAULT '{}',
            error_message VARCHAR,
            started_at TIMESTAMP NOT NULL DEFAULT now(),
            finished_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_standardized_revision ON fundamental_standardized(revision_group_id,available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_standardized_validity ON fundamental_standardized(available_at,valid_to)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_standardization_builds_run ON fundamental_standardization_builds(run_id,status)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (
            'fundamental_standardization_builds','control','fundamental_standardization_build',
            'build_id',
            'Auditable manifests for deterministic standardized-fundamentals materializations.',
            '["build_id"]',
            'rule_set_sha256 and scope_json pin the exact rules and requested entity scope; counts describe the committed output.',
            now()
        )
        """
    )
    conn.execute(
        """
        UPDATE table_catalog
        SET grain = 'security_id,item_id,basis,period_end,available_at',
            description = 'Revision-complete comparable annual, quarterly, instant, and TTM fundamentals produced by the governed standardization rule set.',
            natural_key_json = '["source","security_id","item_id","basis","period_end","available_at"]',
            pit_notes = 'Every visible revision is retained. Select available_at <= decision time, then choose first or latest within revision_group_id; valid_to is the exclusive successor timestamp.',
            updated_at = now()
        WHERE table_name = 'fundamental_standardized'
        """
    )
    _catalog_fields_for_tables(
        conn,
        ("fundamental_standardized", "fundamental_standardization_builds"),
    )

    # This is a pre-production contract replacement: retain only the active schema
    # versions instead of accumulating obsolete rows from earlier snapshots.
    conn.execute("DELETE FROM api_schema_catalog WHERE dataset_id = 'ATX.US.FUNDAMENTALS'")
    conn.execute("DELETE FROM api_field_catalog WHERE dataset_id = 'ATX.US.FUNDAMENTALS'")
    _seed_public_contract(conn)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=271,
        name="standardized_fundamentals_revision_release",
        up=_standardized_fundamentals_release,
    )
]
