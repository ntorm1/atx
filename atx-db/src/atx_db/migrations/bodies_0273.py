"""Governed PIT accounting-identity reconciliation product."""

from __future__ import annotations

import datetime as dt
import hashlib

import duckdb

from ..api.catalog import FUNDAMENTAL_RECONCILIATION_SCHEMA
from ..fundamental_reconciliation import RECONCILIATION_RULES, RECONCILIATION_TERMS
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin
from .bodies_0267 import _seed_public_contract


def _fundamental_reconciliation_release(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_reconciliation_rule (
            rule_id VARCHAR PRIMARY KEY,
            rule_version VARCHAR NOT NULL,
            label VARCHAR NOT NULL,
            statement_type VARCHAR NOT NULL,
            industry_template VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            unit_type VARCHAR NOT NULL,
            tolerance_absolute DOUBLE NOT NULL,
            tolerance_relative DOUBLE NOT NULL,
            mismatch_severity VARCHAR NOT NULL,
            citation VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            is_active BOOLEAN NOT NULL DEFAULT true,
            valid_from DATE NOT NULL DEFAULT DATE '1900-01-01',
            valid_to DATE,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS fundamental_reconciliation_rule_term (
            rule_id VARCHAR NOT NULL,
            term_position INTEGER NOT NULL,
            term_role VARCHAR NOT NULL,
            item_id INTEGER NOT NULL,
            weight DOUBLE NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (rule_id,term_position)
        )
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO fundamental_reconciliation_rule (
            rule_id,rule_version,label,statement_type,industry_template,basis,
            unit_type,tolerance_absolute,tolerance_relative,mismatch_severity,
            citation,description,is_active,valid_from,valid_to,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,now())
        """,
        [
            (
                rule.rule_id,
                rule.rule_version,
                rule.label,
                rule.statement_type,
                rule.industry_template,
                rule.basis,
                rule.unit_type,
                rule.tolerance_absolute,
                rule.tolerance_relative,
                rule.mismatch_severity,
                rule.citation,
                rule.description,
                rule.is_active,
                rule.valid_from,
                rule.valid_to,
            )
            for rule in RECONCILIATION_RULES
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO fundamental_reconciliation_rule_term (
            rule_id,term_position,term_role,item_id,weight,updated_at
        ) VALUES (?,?,?,?,?,now())
        """,
        [
            (term.rule_id, term.term_position, term.term_role, term.item_id, term.weight)
            for term in RECONCILIATION_TERMS
        ],
    )

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_reconciliation AS
        WITH term_counts AS (
            SELECT rule_id,count(*) AS required_term_count
            FROM fundamental_reconciliation_rule_term
            GROUP BY rule_id
        ),
        rule_keys AS (
            SELECT DISTINCT
                definition.rule_id,
                value.security_id,
                value.period_end,
                value.basis
            FROM fundamental_reconciliation_rule definition
            JOIN fundamental_reconciliation_rule_term term
              ON term.rule_id = definition.rule_id
            JOIN fundamental_standardized value
              ON value.item_id = term.item_id
             AND value.basis = definition.basis
             AND definition.valid_from <= value.period_end
             AND coalesce(definition.valid_to, DATE '9999-12-31') > value.period_end
            WHERE definition.is_active
        ),
        events AS (
            SELECT DISTINCT
                definition.rule_id,
                value.security_id,
                value.period_end,
                value.basis,
                value.available_at AS event_at
            FROM fundamental_reconciliation_rule definition
            JOIN fundamental_reconciliation_rule_term term
              ON term.rule_id = definition.rule_id
            JOIN fundamental_standardized value
              ON value.item_id = term.item_id
             AND value.basis = definition.basis
             AND definition.valid_from <= value.period_end
             AND coalesce(definition.valid_to, DATE '9999-12-31') > value.period_end
            WHERE definition.is_active

            UNION

            SELECT
                key.rule_id,
                key.security_id,
                key.period_end,
                key.basis,
                route.available_at AS event_at
            FROM rule_keys key
            JOIN entity_industry_template route
              ON route.security_id = key.security_id
             AND route.valid_from <= key.period_end
             AND coalesce(route.valid_to, DATE '9999-12-31') > key.period_end
        ),
        visible_inputs AS (
            SELECT
                event.rule_id,
                event.security_id,
                event.period_end,
                event.basis,
                event.event_at,
                term.term_position,
                term.term_role,
                term.item_id,
                term.weight,
                value.standardized_id,
                value.symbol,
                value.cik,
                value.period_start,
                value.fiscal_year,
                value.fiscal_period,
                value.value,
                value.as_of_date,
                value.available_at AS input_available_at,
                value.source_loaded_at AS input_source_loaded_at,
                value.run_id,
                row_number() OVER (
                    PARTITION BY event.rule_id,event.security_id,event.period_end,
                                 event.basis,event.event_at,term.term_position
                    ORDER BY value.available_at DESC,value.source_loaded_at DESC,
                             value.standardized_id DESC
                ) AS input_rank
            FROM events event
            JOIN fundamental_reconciliation_rule_term term
              ON term.rule_id = event.rule_id
            JOIN fundamental_standardized value
              ON value.security_id = event.security_id
             AND value.item_id = term.item_id
             AND value.period_end = event.period_end
             AND value.basis = event.basis
             AND value.available_at <= event.event_at
        ),
        picked AS (
            SELECT * EXCLUDE (input_rank)
            FROM visible_inputs
            WHERE input_rank = 1
        ),
        aggregated AS (
            SELECT
                picked.rule_id,
                definition.rule_version,
                definition.label,
                definition.statement_type,
                definition.industry_template AS applicable_template,
                picked.security_id,
                any_value(picked.symbol) AS symbol,
                any_value(picked.cik) AS cik,
                picked.basis,
                min(picked.period_start) AS period_start,
                picked.period_end,
                any_value(picked.fiscal_year) AS fiscal_year,
                any_value(picked.fiscal_period) AS fiscal_period,
                picked.event_at,
                sum(picked.value * picked.weight) FILTER (WHERE picked.term_role='lhs')
                    AS lhs_value,
                sum(picked.value * picked.weight) FILTER (WHERE picked.term_role='rhs')
                    AS rhs_value,
                definition.unit_type,
                definition.tolerance_absolute,
                definition.tolerance_relative,
                definition.mismatch_severity,
                definition.citation,
                definition.description,
                max(picked.as_of_date) AS input_as_of_date,
                max(picked.input_source_loaded_at) AS input_source_loaded_at,
                any_value(picked.run_id) AS run_id,
                CAST(to_json(list(picked.standardized_id ORDER BY picked.term_position)) AS VARCHAR)
                    AS input_standardized_ids_json,
                CAST(to_json(list(picked.item_id ORDER BY picked.term_position)) AS VARCHAR)
                    AS input_item_ids_json,
                CAST(to_json(list(struct_pack(
                    term_position := picked.term_position,
                    term_role := picked.term_role,
                    item_id := picked.item_id,
                    weight := picked.weight,
                    value := picked.value,
                    standardized_id := picked.standardized_id,
                    available_at := picked.input_available_at
                ) ORDER BY picked.term_position)) AS VARCHAR) AS input_values_json
            FROM picked
            JOIN fundamental_reconciliation_rule definition
              ON definition.rule_id = picked.rule_id
            JOIN term_counts counts ON counts.rule_id = picked.rule_id
            GROUP BY
                picked.rule_id,definition.rule_version,definition.label,
                definition.statement_type,definition.industry_template,
                picked.security_id,picked.basis,picked.period_end,picked.event_at,
                definition.unit_type,definition.tolerance_absolute,
                definition.tolerance_relative,definition.mismatch_severity,
                definition.citation,definition.description,
                counts.required_term_count
            HAVING count(DISTINCT picked.term_position) = counts.required_term_count
        ),
        visible_routes AS (
            SELECT
                aggregate.*,
                coalesce(route.industry_template,'ALL') AS industry_template,
                route.as_of_date AS route_as_of_date,
                route.source_loaded_at AS route_source_loaded_at,
                row_number() OVER (
                    PARTITION BY aggregate.rule_id,aggregate.security_id,
                                 aggregate.period_end,aggregate.basis,aggregate.event_at
                    ORDER BY route.available_at DESC NULLS LAST,
                             route.valid_from DESC NULLS LAST,
                             route.source_loaded_at DESC NULLS LAST,
                             route.route_id DESC NULLS LAST
                ) AS route_rank
            FROM aggregated aggregate
            LEFT JOIN entity_industry_template route
              ON route.security_id = aggregate.security_id
             AND route.valid_from <= aggregate.period_end
             AND coalesce(route.valid_to, DATE '9999-12-31') > aggregate.period_end
             AND route.available_at <= aggregate.event_at
        ),
        routed AS (
            SELECT * EXCLUDE (route_rank)
            FROM visible_routes
            WHERE route_rank = 1
        ),
        scored AS (
            SELECT
                routed.*,
                applicable_template='ALL' OR applicable_template=industry_template
                    AS is_applicable,
                lhs_value-rhs_value AS residual,
                abs(lhs_value-rhs_value) AS absolute_difference,
                CASE
                    WHEN greatest(abs(lhs_value),abs(rhs_value)) = 0 THEN NULL
                    ELSE (lhs_value-rhs_value) / greatest(abs(lhs_value),abs(rhs_value))
                END AS residual_percent,
                greatest(
                    tolerance_absolute,
                    tolerance_relative * greatest(abs(lhs_value),abs(rhs_value))
                ) AS tolerance,
                CASE
                    WHEN NOT (
                        applicable_template='ALL' OR applicable_template=industry_template
                    ) THEN 'not_applicable'
                    WHEN abs(lhs_value-rhs_value) <= greatest(
                        tolerance_absolute,
                        tolerance_relative * greatest(abs(lhs_value),abs(rhs_value))
                    ) THEN 'reconciled'
                    WHEN mismatch_severity='diagnostic' THEN 'diagnostic_difference'
                    ELSE 'mismatch'
                END AS status,
                greatest(input_as_of_date,coalesce(route_as_of_date,input_as_of_date))
                    AS as_of_date,
                greatest(
                    input_source_loaded_at,
                    coalesce(route_source_loaded_at,input_source_loaded_at)
                ) AS source_loaded_at,
                sha256(concat_ws('|','fundamental-reconciliation',rule_id,security_id,
                                 basis,CAST(period_end AS VARCHAR))) AS reconciliation_group_id
            FROM routed
        ),
        sequenced AS (
            SELECT
                scored.*,
                row_number() OVER result_window AS revision_sequence,
                count(*) OVER result_window AS revision_count,
                lag(status) OVER result_window AS previous_status,
                lag(industry_template) OVER result_window AS previous_industry_template,
                lag(input_standardized_ids_json) OVER result_window
                    AS previous_input_standardized_ids_json,
                lead(event_at) OVER result_window AS valid_to
            FROM scored
            WINDOW result_window AS (
                PARTITION BY reconciliation_group_id
                ORDER BY event_at,source_loaded_at,input_standardized_ids_json
                ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
            )
        )
        SELECT
            sha256(concat_ws('|',reconciliation_group_id,CAST(event_at AS VARCHAR)))
                AS reconciliation_id,
            'fundamental_reconciliation_v1' AS source,
            security_id,
            symbol,
            cik,
            rule_id,
            rule_version,
            label,
            statement_type,
            industry_template,
            basis,
            period_start,
            period_end,
            fiscal_year,
            fiscal_period,
            lhs_value,
            rhs_value,
            residual,
            absolute_difference,
            residual_percent,
            tolerance,
            status,
            is_applicable,
            mismatch_severity,
            unit_type,
            citation,
            description,
            input_standardized_ids_json,
            input_item_ids_json,
            input_values_json,
            reconciliation_group_id,
            revision_sequence,
            revision_count,
            previous_status,
            CASE
                WHEN revision_sequence=1 THEN 'original'
                WHEN input_standardized_ids_json IS DISTINCT FROM
                     previous_input_standardized_ids_json THEN 'restated'
                WHEN industry_template IS DISTINCT FROM previous_industry_template
                    THEN 'classification_update'
                ELSE 'metadata_update'
            END AS update_type,
            valid_to,
            revision_sequence=revision_count AS is_latest_revision,
            as_of_date,
            event_at AS available_at,
            run_id,
            source_loaded_at
        FROM sequenced
        """
    )

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_reconciliation_rule_basis ON fundamental_reconciliation_rule(basis,industry_template,is_active)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_reconciliation_term_item ON fundamental_reconciliation_rule_term(item_id,rule_id)",
    ):
        conn.execute(statement)

    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "fundamental_reconciliation_rule",
                "reference",
                "fundamental_reconciliation_rule",
                "rule_id",
                "Versioned accounting-identity definitions, applicability, tolerances, and citations.",
                '["rule_id"]',
                "valid_from/valid_to effective-date rule definitions.",
            ),
            (
                "fundamental_reconciliation_rule_term",
                "reference",
                "fundamental_reconciliation_rule_term",
                "rule_id,term_position",
                "Ordered weighted canonical inputs for each reconciliation rule.",
                '["rule_id","term_position"]',
                "Terms inherit the effective window of their parent rule.",
            ),
            (
                "v_fundamental_reconciliation",
                "api",
                "fundamental_reconciliation",
                "security_id,rule_id,basis,period_end,available_at",
                "Revision-complete standardized accounting-identity results with exact input lineage.",
                '["reconciliation_id"]',
                "Each input-availability event is recomputed from the latest visible standardized terms.",
            ),
        ],
    )
    _catalog_fields_for_tables(
        conn,
        (
            "fundamental_reconciliation_rule",
            "fundamental_reconciliation_rule_term",
            "v_fundamental_reconciliation",
        ),
    )
    _seed_public_contract(conn)

    valid_from = dt.datetime(1900, 1, 1)
    natural = (
        f"{FUNDAMENTAL_RECONCILIATION_SCHEMA.dataset}|{FUNDAMENTAL_RECONCILIATION_SCHEMA.code}"
        f"|historical|USD|{valid_from.isoformat()}"
    )
    conn.execute(
        """
        INSERT OR IGNORE INTO api_unit_price_catalog (
            price_id,dataset_id,schema_code,mode,currency,billing_unit,
            unit_price_per_gb,status,valid_from
        ) VALUES (?,?,?,?,?,?,NULL,'contract_required',?)
        """,
        [
            hashlib.sha256(natural.encode()).hexdigest(),
            FUNDAMENTAL_RECONCILIATION_SCHEMA.dataset,
            FUNDAMENTAL_RECONCILIATION_SCHEMA.code,
            "historical",
            "USD",
            "uncompressed_arrow_bytes",
            valid_from,
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=273,
        name="fundamental_reconciliation_product",
        up=_fundamental_reconciliation_release,
    )
]
