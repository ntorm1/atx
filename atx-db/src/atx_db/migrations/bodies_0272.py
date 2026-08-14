"""Revision-complete industry routing and customer-facing dynamic statements."""

from __future__ import annotations

import datetime as dt
import hashlib

import duckdb

from ..api.catalog import INDUSTRY_FUNDAMENTALS_SCHEMA
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin
from .bodies_0267 import _seed_public_contract


def _industry_standardized_release(conn: duckdb.DuckDBPyConnection) -> None:
    for statement in (
        "ALTER TABLE entity_industry_template ADD COLUMN IF NOT EXISTS route_revision_group_id VARCHAR",
        "ALTER TABLE entity_industry_template ADD COLUMN IF NOT EXISTS revision_sequence INTEGER",
        "ALTER TABLE entity_industry_template ADD COLUMN IF NOT EXISTS revision_count INTEGER",
        "ALTER TABLE entity_industry_template ADD COLUMN IF NOT EXISTS previous_industry_template VARCHAR",
        "ALTER TABLE entity_industry_template ADD COLUMN IF NOT EXISTS update_type VARCHAR",
        "ALTER TABLE entity_industry_template ADD COLUMN IF NOT EXISTS knowledge_valid_to TIMESTAMP",
    ):
        conn.execute(statement)

    conn.execute(
        """
        CREATE TEMP TABLE _industry_route_revision_backfill AS
        SELECT
            route_id,
            sha256(concat_ws('|', source, security_id, CAST(valid_from AS VARCHAR)))
                AS route_revision_group_id,
            row_number() OVER route_window AS revision_sequence,
            count(*) OVER route_window AS revision_count,
            lag(industry_template) OVER route_window AS previous_industry_template,
            lead(available_at) OVER route_window AS knowledge_valid_to
        FROM entity_industry_template
        WINDOW route_window AS (
            PARTITION BY source,security_id,valid_from
            ORDER BY available_at,source_loaded_at,route_id
            ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
        )
        """
    )
    conn.execute(
        """
        UPDATE entity_industry_template AS target
        SET
            route_revision_group_id = source.route_revision_group_id,
            revision_sequence = source.revision_sequence,
            revision_count = source.revision_count,
            previous_industry_template = source.previous_industry_template,
            update_type = CASE
                WHEN source.revision_sequence = 1 THEN 'original' ELSE 'restated'
            END,
            knowledge_valid_to = source.knowledge_valid_to,
            is_latest_revision = source.revision_sequence = source.revision_count
        FROM _industry_route_revision_backfill AS source
        WHERE target.route_id = source.route_id
        """
    )
    conn.execute("DROP TABLE _industry_route_revision_backfill")

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_industry_standardized AS
        WITH standardization_keys AS (
            SELECT
                coalesce(revision_group_id,standardized_id) AS value_revision_group_id,
                security_id,
                item_id,
                basis,
                period_end,
                min(available_at) AS first_value_available_at
            FROM fundamental_standardized
            GROUP BY 1,2,3,4,5
        ),
        product_events AS (
            SELECT
                coalesce(revision_group_id,standardized_id) AS value_revision_group_id,
                available_at AS event_at
            FROM fundamental_standardized

            UNION

            SELECT
                key.value_revision_group_id,
                route.available_at AS event_at
            FROM standardization_keys key
            JOIN entity_industry_template route
              ON route.security_id = key.security_id
             AND route.valid_from <= key.period_end
             AND coalesce(route.valid_to, DATE '9999-12-31') > key.period_end
             AND route.available_at >= key.first_value_available_at
        ),
        visible_values AS (
            SELECT
                event.value_revision_group_id,
                event.event_at,
                value.*,
                row_number() OVER (
                    PARTITION BY event.value_revision_group_id,event.event_at
                    ORDER BY value.available_at DESC,value.source_loaded_at DESC,
                             value.standardized_id DESC
                ) AS value_rank
            FROM product_events event
            JOIN fundamental_standardized value
              ON coalesce(value.revision_group_id,value.standardized_id)
                    = event.value_revision_group_id
             AND value.available_at <= event.event_at
        ),
        picked_values AS (
            SELECT * EXCLUDE (value_rank)
            FROM visible_values
            WHERE value_rank = 1
        ),
        visible_routes AS (
            SELECT
                picked.*,
                route.route_id,
                route.source AS route_source,
                route.industry_template AS routed_template,
                route.matched_taxonomy,
                route.matched_node_code,
                route.match_reason AS route_match_reason,
                route.valid_from AS route_valid_from,
                route.valid_to AS route_valid_to,
                route.as_of_date AS route_as_of_date,
                route.available_at AS route_available_at,
                route.source_loaded_at AS route_source_loaded_at,
                row_number() OVER (
                    PARTITION BY picked.value_revision_group_id,picked.event_at
                    ORDER BY route.available_at DESC NULLS LAST,
                             route.valid_from DESC NULLS LAST,
                             route.source_loaded_at DESC NULLS LAST,
                             route.route_id DESC NULLS LAST
                ) AS route_rank
            FROM picked_values picked
            LEFT JOIN entity_industry_template route
              ON route.security_id = picked.security_id
             AND route.valid_from <= picked.period_end
             AND coalesce(route.valid_to, DATE '9999-12-31') > picked.period_end
             AND route.available_at <= picked.event_at
        ),
        picked_routes AS (
            SELECT * EXCLUDE (route_rank)
            FROM visible_routes
            WHERE route_rank = 1
        ),
        enriched AS (
            SELECT
                routed.*,
                coalesce(routed.routed_template,'ALL') AS industry_template,
                template.label AS template_label,
                template.vendor_profile,
                template.accounting_class,
                coalesce(specific.requirement_level,core.requirement_level,'supplemental')
                    AS requirement_level,
                coalesce(specific.not_available,core.not_available,false)
                    AS template_not_available,
                coalesce(routed.route_match_reason,'default_all_no_visible_route')
                    AS template_match_reason,
                greatest(
                    routed.as_of_date,
                    coalesce(routed.route_as_of_date,routed.as_of_date)
                ) AS product_as_of_date,
                greatest(
                    routed.source_loaded_at,
                    coalesce(routed.route_source_loaded_at,routed.source_loaded_at)
                ) AS product_source_loaded_at,
                routed.available_at AS standardized_available_at
            FROM picked_routes routed
            JOIN industry_template template
              ON template.template_code = coalesce(routed.routed_template,'ALL')
            LEFT JOIN industry_template_item specific
              ON specific.template_code = coalesce(routed.routed_template,'ALL')
             AND specific.item_id = routed.item_id
             AND specific.valid_from <= routed.period_end
             AND coalesce(specific.valid_to, DATE '9999-12-31') > routed.period_end
            LEFT JOIN industry_template_item core
              ON core.template_code = 'ALL'
             AND core.item_id = routed.item_id
             AND core.valid_from <= routed.period_end
             AND coalesce(core.valid_to, DATE '9999-12-31') > routed.period_end
        ),
        sequenced AS (
            SELECT
                enriched.*,
                sha256(concat_ws('|','industry-standardized',value_revision_group_id))
                    AS industry_revision_group_id,
                row_number() OVER product_window AS product_revision_sequence,
                count(*) OVER product_window AS product_revision_count,
                lag(value) OVER product_window AS product_previous_value,
                lag(industry_template) OVER product_window AS previous_industry_template,
                lead(event_at) OVER product_window AS product_valid_to
            FROM enriched
            WINDOW product_window AS (
                PARTITION BY value_revision_group_id
                ORDER BY event_at,product_source_loaded_at,standardized_id
                ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
            )
        )
        SELECT
            sha256(concat_ws('|',industry_revision_group_id,CAST(event_at AS VARCHAR)))
                AS industry_standardized_id,
            standardized_id,
            source,
            upstream_source,
            security_id,
            symbol,
            cik,
            item_id,
            canonical_code,
            basis,
            period_start,
            period_end,
            fiscal_year,
            fiscal_period,
            value,
            unit,
            unit_type,
            source_accession,
            filed_date,
            product_as_of_date AS as_of_date,
            event_at AS available_at,
            input_codes_json,
            input_item_ids_json,
            rule_id,
            combination_rule,
            industry_revision_group_id,
            product_revision_sequence AS revision_sequence,
            product_revision_count AS revision_count,
            CASE
                WHEN product_revision_sequence = 1 THEN false
                ELSE value IS DISTINCT FROM product_previous_value
            END AS is_value_changed,
            product_previous_value AS previous_value,
            CASE
                WHEN product_previous_value IS NULL THEN NULL
                ELSE value - product_previous_value
            END AS value_delta,
            CASE
                WHEN product_previous_value IS NULL OR product_previous_value = 0 THEN NULL
                ELSE (value - product_previous_value) / abs(product_previous_value)
            END AS value_delta_percent,
            CASE
                WHEN product_revision_sequence = 1 THEN 'original'
                WHEN value IS DISTINCT FROM product_previous_value THEN 'restated'
                WHEN industry_template IS DISTINCT FROM previous_industry_template
                    THEN 'classification_update'
                ELSE 'metadata_update'
            END AS update_type,
            product_valid_to AS valid_to,
            product_revision_sequence = product_revision_count AS is_latest_revision,
            run_id,
            product_source_loaded_at AS source_loaded_at,
            industry_template,
            template_label,
            vendor_profile,
            accounting_class,
            requirement_level,
            template_not_available,
            template_match_reason,
            matched_taxonomy,
            matched_node_code,
            route_id,
            route_source,
            route_valid_from,
            route_valid_to,
            route_available_at,
            standardized_available_at,
            value_revision_group_id
        FROM sequenced
        """
    )

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_entity_industry_template_revision ON entity_industry_template(route_revision_group_id,available_at)",
        "CREATE INDEX IF NOT EXISTS idx_entity_industry_template_pit ON entity_industry_template(security_id,available_at,valid_from,valid_to)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        UPDATE table_catalog
        SET grain = 'security_id,valid_from,available_at',
            description = 'Revision-complete PIT security-to-industry statement-template routing.',
            natural_key_json = '["source","security_id","valid_from","available_at"]',
            pit_notes = 'valid_from/valid_to are economic validity; available_at/knowledge_valid_to are knowledge validity.',
            updated_at = now()
        WHERE table_name = 'entity_industry_template'
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (
            'v_fundamental_industry_standardized','api','industry_standardized_fundamental',
            'security_id,item_id,basis,period_end,available_at',
            'Dynamic industry-routed standardized statements with value and classification revisions.',
            '["industry_standardized_id"]',
            'Events combine standardized-value and industry-route availability; select available_at <= decision time.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(
        conn,
        ("entity_industry_template", "v_fundamental_industry_standardized"),
    )
    _seed_public_contract(conn)

    valid_from = dt.datetime(1900, 1, 1)
    natural = (
        f"{INDUSTRY_FUNDAMENTALS_SCHEMA.dataset}|{INDUSTRY_FUNDAMENTALS_SCHEMA.code}"
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
            INDUSTRY_FUNDAMENTALS_SCHEMA.dataset,
            INDUSTRY_FUNDAMENTALS_SCHEMA.code,
            "historical",
            "USD",
            "uncompressed_arrow_bytes",
            valid_from,
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=272,
        name="industry_standardized_dynamic_statement_release",
        up=_industry_standardized_release,
    )
]
