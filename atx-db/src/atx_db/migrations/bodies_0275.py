"""Filing-context verification for accounting reconciliation results."""

from __future__ import annotations

import duckdb

from ..fundamental_reconciliation import RECONCILIATION_RULES, RECONCILIATION_TERMS
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin
from .bodies_0267 import _seed_public_contract


def _fundamental_reconciliation_context_verification(
    conn: duckdb.DuckDBPyConnection,
) -> None:
    conn.execute("DROP VIEW IF EXISTS v_fundamental_reconciliation")
    conn.execute("DROP INDEX IF EXISTS idx_fundamental_reconciliation_term_item")
    conn.execute(
        """
        ALTER TABLE fundamental_reconciliation_rule_term
        ADD COLUMN IF NOT EXISTS is_required BOOLEAN DEFAULT true
        """
    )
    conn.execute(
        """
        DELETE FROM fundamental_reconciliation_rule_term
        WHERE rule_id='assets_equal_liabilities_equity_instant';
        DELETE FROM fundamental_reconciliation_rule
        WHERE rule_id='assets_equal_liabilities_equity_instant'
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
            rule_id,term_position,term_role,item_id,weight,is_required,updated_at
        ) VALUES (?,?,?,?,?,?,now())
        """,
        [
            (
                term.rule_id,
                term.term_position,
                term.term_role,
                term.item_id,
                term.weight,
                term.is_required,
            )
            for term in RECONCILIATION_TERMS
        ],
    )

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_reconciliation AS
        WITH term_counts AS (
            SELECT
                rule_id,
                count(*) FILTER (WHERE is_required) AS required_term_count
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
                term.is_required,
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
                value.source_accession,
                value.filed_date,
                value.input_codes_json,
                value.upstream_source,
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
                count(*) AS input_term_count,
                count(DISTINCT picked.source_accession)
                    FILTER (WHERE picked.source_accession IS NOT NULL)
                    AS input_accession_count,
                count(*) FILTER (WHERE picked.source_accession IS NULL)
                    AS null_accession_count,
                coalesce(
                    CAST(to_json(list(DISTINCT picked.source_accession ORDER BY picked.source_accession)
                        FILTER (WHERE picked.source_accession IS NOT NULL)) AS VARCHAR),
                    '[]'
                ) AS input_accessions_json,
                CAST(to_json(list(picked.standardized_id ORDER BY picked.term_position)) AS VARCHAR)
                    AS input_standardized_ids_json,
                CAST(to_json(list(picked.item_id ORDER BY picked.term_position)) AS VARCHAR)
                    AS input_item_ids_json,
                CAST(to_json(list(struct_pack(
                    term_position := picked.term_position,
                    term_role := picked.term_role,
                    item_id := picked.item_id,
                    weight := picked.weight,
                    is_required := picked.is_required,
                    value := picked.value,
                    standardized_id := picked.standardized_id,
                    accession_number := picked.source_accession,
                    filed_date := picked.filed_date,
                    input_codes_json := picked.input_codes_json,
                    upstream_source := picked.upstream_source,
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
            HAVING count(DISTINCT picked.term_position)
                FILTER (WHERE picked.is_required) = counts.required_term_count
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
        input_context_candidates AS (
            SELECT
                picked.rule_id,
                picked.security_id,
                picked.period_end,
                picked.basis,
                picked.event_at,
                picked.term_position,
                fact.accession_number,
                fact.primary_document,
                fact.filing_context_id,
                context.context_id,
                context.context_hash,
                context.dimension_count,
                fact.unit_ref,
                fact.unit_measures_json,
                fact.acceptance_datetime,
                fact.filing_fact_id
            FROM picked
            JOIN xbrl_filing_facts fact
              ON fact.security_id = picked.security_id
             AND fact.accession_number = picked.source_accession
             AND fact.taxonomy = split_part(
                    json_extract_string(picked.input_codes_json,'$[0]'),':',1
                 )
             AND fact.concept = split_part(
                    json_extract_string(picked.input_codes_json,'$[0]'),':',2
                 )
             AND fact.is_numeric
             AND fact.numeric_value IS NOT NULL
             AND fact.acceptance_datetime <= picked.event_at
             AND abs(abs(fact.numeric_value)-abs(picked.value)) <= greatest(
                    1.0, 0.000000000001 * abs(picked.value)
                 )
            JOIN xbrl_filing_contexts context
              ON context.filing_context_id = fact.filing_context_id
             AND context.security_id = fact.security_id
             AND context.accession_number = fact.accession_number
             AND context.primary_document = fact.primary_document
             AND (
                    (picked.basis='instant' AND context.period_type='instant'
                        AND context.instant_date=picked.period_end)
                 OR (picked.basis<>'instant' AND context.period_type='duration'
                        AND context.period_start IS NOT DISTINCT FROM picked.period_start
                        AND context.period_end=picked.period_end)
                 )
            WHERE json_valid(picked.input_codes_json)
              AND json_array_length(picked.input_codes_json)=1
              AND strpos(json_extract_string(picked.input_codes_json,'$[0]'),':') > 0
        ),
        shared_contexts AS (
            SELECT
                candidate.rule_id,
                candidate.security_id,
                candidate.period_end,
                candidate.basis,
                candidate.event_at,
                candidate.accession_number,
                candidate.primary_document,
                candidate.filing_context_id,
                candidate.context_id,
                candidate.context_hash,
                candidate.dimension_count,
                candidate.unit_ref,
                any_value(candidate.unit_measures_json) AS unit_measures_json,
                max(candidate.acceptance_datetime) AS acceptance_datetime,
                count(DISTINCT candidate.term_position) AS matched_term_count,
                CAST(to_json(list(DISTINCT candidate.filing_fact_id
                    ORDER BY candidate.filing_fact_id)) AS VARCHAR) AS filing_fact_ids_json
            FROM input_context_candidates candidate
            GROUP BY
                candidate.rule_id,candidate.security_id,candidate.period_end,
                candidate.basis,candidate.event_at,candidate.accession_number,
                candidate.primary_document,candidate.filing_context_id,
                candidate.context_id,candidate.context_hash,
                candidate.dimension_count,candidate.unit_ref
        ),
        verified_contexts AS (
            SELECT * EXCLUDE (context_rank)
            FROM (
                SELECT
                    shared.*,
                    row_number() OVER (
                        PARTITION BY shared.rule_id,shared.security_id,
                                     shared.period_end,shared.basis,shared.event_at
                        ORDER BY shared.dimension_count,shared.acceptance_datetime DESC,
                                 shared.filing_context_id,shared.unit_ref
                    ) AS context_rank
                FROM shared_contexts shared
                JOIN routed
                  ON routed.rule_id=shared.rule_id
                 AND routed.security_id=shared.security_id
                 AND routed.period_end=shared.period_end
                 AND routed.basis=shared.basis
                 AND routed.event_at=shared.event_at
                 AND routed.input_accession_count=1
                 AND routed.null_accession_count=0
                 AND shared.matched_term_count=routed.input_term_count
            )
            WHERE context_rank=1
        ),
        filing_presence AS (
            SELECT DISTINCT
                routed.rule_id,
                routed.security_id,
                routed.period_end,
                routed.basis,
                routed.event_at
            FROM routed
            JOIN picked
              ON picked.rule_id=routed.rule_id
             AND picked.security_id=routed.security_id
             AND picked.period_end=routed.period_end
             AND picked.basis=routed.basis
             AND picked.event_at=routed.event_at
            JOIN xbrl_filing_facts fact
              ON fact.security_id=picked.security_id
             AND fact.accession_number=picked.source_accession
             AND fact.acceptance_datetime <= picked.event_at
            WHERE routed.input_accession_count=1
              AND routed.null_accession_count=0
        ),
        context_scored AS (
            SELECT
                routed.*,
                CASE
                    WHEN routed.null_accession_count > 0 THEN 'unknown_accession'
                    WHEN routed.input_accession_count = 1 THEN 'single_filing'
                    WHEN routed.input_accession_count > 1 THEN 'mixed_filing_vintage'
                    ELSE 'unknown_accession'
                END AS input_filing_status,
                CASE
                    WHEN routed.null_accession_count > 0 OR routed.input_accession_count=0
                        THEN 'unknown_accession'
                    WHEN routed.input_accession_count > 1 THEN 'mixed_filing_vintage'
                    WHEN verified.filing_context_id IS NOT NULL THEN 'verified_same_context'
                    WHEN presence.rule_id IS NULL THEN 'context_not_loaded'
                    ELSE 'context_not_aligned'
                END AS context_verification_status,
                verified.filing_context_id AS verified_filing_context_id,
                CAST(to_json(struct_pack(
                    accession_number := verified.accession_number,
                    primary_document := verified.primary_document,
                    filing_context_id := verified.filing_context_id,
                    context_id := verified.context_id,
                    context_hash := verified.context_hash,
                    dimension_count := verified.dimension_count,
                    unit_ref := verified.unit_ref,
                    unit_measures_json := verified.unit_measures_json,
                    acceptance_datetime := verified.acceptance_datetime,
                    matched_term_count := coalesce(verified.matched_term_count,0),
                    input_term_count := routed.input_term_count,
                    filing_fact_ids_json := coalesce(verified.filing_fact_ids_json,'[]')
                )) AS VARCHAR) AS context_evidence_json
            FROM routed
            LEFT JOIN verified_contexts verified
              ON verified.rule_id=routed.rule_id
             AND verified.security_id=routed.security_id
             AND verified.period_end=routed.period_end
             AND verified.basis=routed.basis
             AND verified.event_at=routed.event_at
            LEFT JOIN filing_presence presence
              ON presence.rule_id=routed.rule_id
             AND presence.security_id=routed.security_id
             AND presence.period_end=routed.period_end
             AND presence.basis=routed.basis
             AND presence.event_at=routed.event_at
        ),
        scored AS (
            SELECT
                context_scored.*,
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
            FROM context_scored
        ),
        interpreted AS (
            SELECT
                scored.*,
                status='mismatch'
                    AND mismatch_severity='error'
                    AND context_verification_status='verified_same_context'
                    AS is_hard_failure,
                CASE
                    WHEN status='reconciled' THEN 'within_tolerance'
                    WHEN status='not_applicable' THEN 'rule_not_applicable'
                    WHEN status='diagnostic_difference' THEN 'diagnostic_rule_difference'
                    WHEN context_verification_status='mixed_filing_vintage'
                        THEN 'mixed_filing_vintage'
                    WHEN context_verification_status='context_not_loaded'
                        THEN 'context_not_loaded'
                    WHEN context_verification_status='context_not_aligned'
                        THEN 'xbrl_context_not_aligned'
                    WHEN context_verification_status='verified_same_context'
                        THEN 'verified_accounting_mismatch'
                    ELSE 'unknown_accession'
                END AS mismatch_reason
            FROM scored
        ),
        sequenced AS (
            SELECT
                interpreted.*,
                row_number() OVER result_window AS revision_sequence,
                count(*) OVER result_window AS revision_count,
                lag(status) OVER result_window AS previous_status,
                lag(industry_template) OVER result_window AS previous_industry_template,
                lag(input_standardized_ids_json) OVER result_window
                    AS previous_input_standardized_ids_json,
                lead(event_at) OVER result_window AS valid_to
            FROM interpreted
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
            input_filing_status,
            input_accession_count,
            input_accessions_json,
            context_verification_status,
            verified_filing_context_id,
            context_evidence_json,
            mismatch_reason,
            is_hard_failure,
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
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_fundamental_reconciliation_term_item
        ON fundamental_reconciliation_rule_term(item_id,rule_id)
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (
            'v_fundamental_reconciliation','api','fundamental_reconciliation',
            'security_id,rule_id,basis,period_end,available_at',
            'Revision-complete accounting identities with filing-context verification and exact input lineage.',
            '["reconciliation_id"]',
            'Numerical status is distinct from hard-failure status; hard failures require one accession, XBRL context, dimensional signature, and unit.',
            now()
        )
        """
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
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=275,
        name="fundamental_reconciliation_context_verification",
        up=_fundamental_reconciliation_context_verification,
    )
]
