from __future__ import annotations

from ._types import SqlQualityCheck


def fundamental_check_specs(
    *,
    daily_macro_stale_days: int,
    monthly_macro_stale_days: int,
    valuation_stale_gap_days: int,
) -> tuple[SqlQualityCheck, ...]:
    return (
        SqlQualityCheck(
            dataset_id="xbrl_concept_catalog",
            table_name="xbrl_concept_catalog",
            check_name="duplicate_xbrl_concept_catalog_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, taxonomy, concept, count(*) AS row_count
                    FROM xbrl_concept_catalog
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_concept_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_concept_catalog",
            table_name="xbrl_concept_catalog",
            check_name="bad_xbrl_concept_catalog_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_concept_catalog
                WHERE source IS NULL
                   OR source = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR units_json IS NULL
                   OR units_json IN ('', '[]')
                   OR forms_json IS NULL
                   OR forms_json IN ('', '[]')
                   OR fiscal_periods_json IS NULL
                   OR statement_category NOT IN ('balance_sheet', 'income_statement', 'cash_flow', 'per_share', 'share_count', 'bank_statement', 'insurance_statement', 'reit_statement', 'utility_statement', 'broker_dealer_statement', 'other')
                   OR fact_count <= 0
                   OR security_count <= 0
                   OR accession_count <= 0
                   OR (first_period_end IS NOT NULL AND last_period_end < first_period_end)
                   OR (first_filed_date IS NOT NULL AND last_filed_date < first_filed_date)
                   OR (first_available_at IS NOT NULL AND last_available_at < first_available_at)
            """,
            threshold=0.0,
            required_tables=("xbrl_concept_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_concept_catalog",
            table_name="xbrl_concept_catalog",
            check_name="sec_company_fact_concepts_without_catalog",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT source, taxonomy, concept
                    FROM sec_company_facts
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND concept IS NOT NULL
                      AND concept <> ''
                ) f
                LEFT JOIN xbrl_concept_catalog c
                  ON c.source = f.source
                 AND c.taxonomy = f.taxonomy
                 AND c.concept = f.concept
                WHERE c.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("sec_company_facts", "xbrl_concept_catalog"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_packages",
            check_name="duplicate_xbrl_taxonomy_packages",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT taxonomy_package_id, count(*) AS row_count
                    FROM xbrl_taxonomy_packages
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_taxonomy_packages",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_packages",
            check_name="bad_xbrl_taxonomy_packages",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_taxonomy_packages
                WHERE taxonomy_package_id IS NULL
                   OR taxonomy_package_id = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR release_year < 2000
                   OR source_url IS NULL
                   OR source_url = ''
                   OR package_sha256 IS NULL
                   OR length(package_sha256) <> 64
                   OR byte_count <= 0
                   OR file_count <= 0
                   OR linkbase_file_count <= 0
                   OR relationship_count <= 0
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_taxonomy_packages",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_relationships",
            check_name="duplicate_xbrl_taxonomy_relationships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT relationship_id, count(*) AS row_count
                    FROM xbrl_taxonomy_relationships
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_taxonomy_relationships",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_relationships",
            check_name="bad_xbrl_taxonomy_relationships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_taxonomy_relationships
                WHERE relationship_id IS NULL
                   OR relationship_id = ''
                   OR taxonomy_package_id IS NULL
                   OR taxonomy_package_id = ''
                   OR linkbase_type NOT IN ('presentation', 'calculation', 'definition')
                   OR source_file IS NULL
                   OR source_file = ''
                   OR parent_concept IS NULL
                   OR parent_concept = ''
                   OR child_concept IS NULL
                   OR child_concept = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
                   OR (linkbase_type = 'calculation' AND weight IS NULL)
            """,
            threshold=0.0,
            required_tables=("xbrl_taxonomy_relationships",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_relationships",
            check_name="observed_xbrl_concepts_without_taxonomy_relationships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_concept_catalog c
                LEFT JOIN xbrl_taxonomy_relationships r
                  ON (
                        r.parent_taxonomy = c.taxonomy
                    AND r.parent_concept = c.concept
                  )
                  OR (
                        r.child_taxonomy = c.taxonomy
                    AND r.child_concept = c.concept
                  )
                WHERE c.taxonomy IN ('us-gaap', 'srt')
                  AND r.relationship_id IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_concept_catalog", "xbrl_taxonomy_relationships"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_dimensions",
            table_name="xbrl_dimension_edges",
            check_name="duplicate_xbrl_dimension_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT dimension_edge_id, count(*) AS row_count
                    FROM xbrl_dimension_edges
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_dimension_edges",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_dimensions",
            table_name="xbrl_dimension_edges",
            check_name="bad_xbrl_dimension_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_dimension_edges
                WHERE dimension_edge_id IS NULL
                   OR dimension_edge_id = ''
                   OR relationship_id IS NULL
                   OR relationship_id = ''
                   OR taxonomy_package_id IS NULL
                   OR taxonomy_package_id = ''
                   OR relationship_kind IS NULL
                   OR relationship_kind = ''
                   OR source_concept IS NULL
                   OR source_concept = ''
                   OR target_concept IS NULL
                   OR target_concept = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_dimension_edges",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_dimensions",
            table_name="xbrl_dimension_edges",
            check_name="dimension_edges_without_relationship",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_dimension_edges d
                LEFT JOIN xbrl_taxonomy_relationships r
                  ON r.relationship_id = d.relationship_id
                WHERE r.relationship_id IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_dimension_edges", "xbrl_taxonomy_relationships"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_fact_frames",
            table_name="xbrl_fact_frames",
            check_name="duplicate_xbrl_fact_frames",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, taxonomy, concept, unit, frame, count(*) AS row_count
                    FROM xbrl_fact_frames
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_fact_frames",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_fact_frames",
            table_name="xbrl_fact_frames",
            check_name="bad_xbrl_fact_frames",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_fact_frames
                WHERE fact_frame_id IS NULL
                   OR fact_frame_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR frame IS NULL
                   OR frame = ''
                   OR frame_period IS NULL
                   OR frame_period = ''
                   OR fact_count <= 0
                   OR security_count <= 0
                   OR accession_count <= 0
                   OR (frame_quarter IS NOT NULL AND (frame_quarter < 1 OR frame_quarter > 4))
                   OR (first_period_start IS NOT NULL AND last_period_end < first_period_start)
                   OR (first_filed_date IS NOT NULL AND last_filed_date < first_filed_date)
                   OR (first_available_at IS NOT NULL AND last_available_at < first_available_at)
            """,
            threshold=0.0,
            required_tables=("xbrl_fact_frames",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_fact_frames",
            table_name="xbrl_fact_frames",
            check_name="xbrl_fact_frames_without_sec_company_facts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_fact_frames f
                LEFT JOIN sec_company_facts s
                  ON s.source = f.source
                 AND s.taxonomy = f.taxonomy
                 AND s.concept = f.concept
                 AND s.unit = f.unit
                 AND s.frame = f.frame
                WHERE s.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_fact_frames", "sec_company_facts"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_contexts",
            check_name="duplicate_xbrl_filing_contexts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, accession_number, primary_document, context_id, count(*) AS row_count
                    FROM xbrl_filing_contexts
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_contexts",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_contexts",
            check_name="bad_xbrl_filing_context_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_contexts
                WHERE filing_context_id IS NULL
                   OR filing_context_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR primary_document IS NULL
                   OR primary_document = ''
                   OR context_id IS NULL
                   OR context_id = ''
                   OR period_type NOT IN ('instant', 'duration', 'forever', 'unknown')
                   OR (period_type = 'instant' AND instant_date IS NULL)
                   OR (period_type = 'duration' AND (period_start IS NULL OR period_end IS NULL))
                   OR explicit_member_count < 0
                   OR typed_member_count < 0
                   OR dimension_count <> explicit_member_count + typed_member_count
                   OR context_hash IS NULL
                   OR context_hash = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_contexts",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_contexts",
            check_name="xbrl_filing_contexts_without_sec_submission",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_contexts c
                LEFT JOIN sec_submissions s
                  ON s.security_id = c.security_id
                 AND s.accession_number = c.accession_number
                 AND s.primary_document = c.primary_document
                WHERE s.accession_number IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_contexts", "sec_submissions"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_dimensions",
            check_name="duplicate_xbrl_filing_dimensions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT filing_dimension_id, count(*) AS row_count
                    FROM xbrl_filing_dimensions
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_dimensions",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_dimensions",
            check_name="bad_xbrl_filing_dimension_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_dimensions
                WHERE filing_dimension_id IS NULL
                   OR filing_dimension_id = ''
                   OR filing_context_id IS NULL
                   OR filing_context_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR primary_document IS NULL
                   OR primary_document = ''
                   OR context_id IS NULL
                   OR context_id = ''
                   OR context_element NOT IN ('segment', 'scenario', 'unknown')
                   OR member_kind NOT IN ('explicit', 'typed')
                   OR dimension_qname IS NULL
                   OR dimension_qname = ''
                   OR dimension_concept IS NULL
                   OR dimension_concept = ''
                   OR (member_kind = 'explicit' AND (member_qname IS NULL OR member_qname = '' OR member_concept IS NULL OR member_concept = ''))
                   OR (member_kind = 'typed' AND (typed_member_value IS NULL OR typed_member_value = ''))
                   OR ordinal < 1
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_dimensions",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_dimensions",
            check_name="xbrl_filing_dimensions_without_context",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_dimensions d
                LEFT JOIN xbrl_filing_contexts c
                  ON c.filing_context_id = d.filing_context_id
                WHERE c.filing_context_id IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_dimensions", "xbrl_filing_contexts"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_facts",
            table_name="xbrl_filing_facts",
            check_name="duplicate_xbrl_filing_facts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT filing_fact_id, count(*) AS row_count
                    FROM xbrl_filing_facts
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_facts",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_facts",
            table_name="xbrl_filing_facts",
            check_name="bad_xbrl_filing_fact_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_facts
                WHERE filing_fact_id IS NULL
                   OR filing_fact_id = ''
                   OR filing_context_id IS NULL
                   OR filing_context_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR primary_document IS NULL
                   OR primary_document = ''
                   OR fact_ordinal < 1
                   OR fact_kind NOT IN ('nonFraction', 'nonNumeric', 'fraction')
                   OR qname IS NULL
                   OR qname = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR context_ref IS NULL
                   OR context_ref = ''
                   OR unit_measures_json IS NULL
                   OR unit_numerator_measures_json IS NULL
                   OR unit_denominator_measures_json IS NULL
                   OR is_numeric <> (fact_kind IN ('nonFraction', 'fraction'))
                   OR (is_numeric AND raw_value IS NOT NULL AND raw_value <> '' AND numeric_value IS NULL AND fact_kind = 'nonFraction' AND regexp_matches(replace(replace(raw_value, ',', ''), '$', ''), '^[[:space:]]*[-+]?[0-9]+(\\.[0-9]+)?[[:space:]]*$'))
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_facts",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_facts",
            table_name="xbrl_filing_facts",
            check_name="xbrl_filing_facts_without_context",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_facts f
                LEFT JOIN xbrl_filing_contexts c
                  ON c.filing_context_id = f.filing_context_id
                 AND c.context_id = f.context_ref
                WHERE c.filing_context_id IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_facts", "xbrl_filing_contexts"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_facts",
            table_name="xbrl_filing_facts",
            check_name="xbrl_filing_facts_without_sec_submission",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_facts f
                LEFT JOIN sec_submissions s
                  ON s.security_id = f.security_id
                 AND s.accession_number = f.accession_number
                 AND s.primary_document = f.primary_document
                WHERE s.accession_number IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_facts", "sec_submissions"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_validation",
            table_name="xbrl_validation_results",
            check_name="duplicate_xbrl_validation_results",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT validation_id, count(*) AS row_count
                    FROM xbrl_validation_results
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_validation_results",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_validation",
            table_name="xbrl_validation_results",
            check_name="bad_xbrl_validation_result_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_validation_results
                WHERE validation_id IS NULL
                   OR validation_id = ''
                   OR validation_run_id IS NULL
                   OR validation_run_id = ''
                   OR rule_family NOT IN ('calculation_linkbase', 'dqc')
                   OR rule_code IS NULL
                   OR rule_code = ''
                   OR severity NOT IN ('info', 'warning', 'error')
                   OR status NOT IN ('passed', 'failed', 'warning', 'skipped')
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR primary_document IS NULL
                   OR primary_document = ''
                   OR parent_concept IS NULL
                   OR parent_concept = ''
                   OR context_ref IS NULL
                   OR context_ref = ''
                   OR tolerance < 0
                   OR child_count < 0
                   OR child_facts_json IS NULL
                   OR child_facts_json = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
                   OR resolution_status IS NULL
                   OR resolution_status NOT IN (
                        'resolved_ok',
                        'resolved_dimensional_artifact',
                        'genuine_error',
                        'unresolved'
                   )
                   OR (rule_family = 'calculation_linkbase' AND parent_value IS NULL)
                   OR (rule_family = 'calculation_linkbase' AND child_weighted_sum IS NULL)
                   OR (rule_family = 'calculation_linkbase' AND absolute_difference IS NULL)
                   OR (
                        status = 'passed'
                        AND absolute_difference > tolerance
                        AND resolution_status <> 'resolved_dimensional_artifact'
                   )
                   OR (status = 'failed' AND absolute_difference <= tolerance)
                   OR (
                        rule_family = 'calculation_linkbase'
                        AND status = 'passed'
                        AND resolution_status NOT IN ('resolved_ok', 'resolved_dimensional_artifact')
                   )
                   OR (
                        rule_family = 'calculation_linkbase'
                        AND status = 'failed'
                        AND resolution_status <> 'genuine_error'
                   )
                   OR (
                        rule_family = 'dqc'
                        AND status = 'failed'
                        AND resolution_status <> 'genuine_error'
                   )
            """,
            threshold=0.0,
            required_tables=("xbrl_validation_results",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_validation",
            table_name="xbrl_validation_results",
            check_name="failed_xbrl_calculation_linkbase_checks",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_validation_results
                WHERE rule_family = 'calculation_linkbase'
                  AND resolution_status = 'genuine_error'
            """,
            threshold=0.0,
            required_tables=("xbrl_validation_results",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="duplicate_fundamental_fact_revision_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        taxonomy,
                        concept,
                        unit,
                        period_start,
                        period_end,
                        accession_number,
                        count(*) AS row_count
                    FROM fundamental_fact_revisions
                    GROUP BY 1, 2, 3, 4, 5, 6, 7, 8
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_fact_revisions",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="bad_fundamental_fact_revision_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_fact_revisions
                WHERE fact_revision_id IS NULL
                   OR fact_revision_id = ''
                   OR revision_group_id IS NULL
                   OR revision_group_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR period_end IS NULL
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR filed_date IS NULL
                   OR source_url IS NULL
                   OR source_url = ''
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR (revision_sequence = 1 AND previous_accession_number IS NOT NULL)
                   OR (revision_sequence > 1 AND previous_accession_number IS NULL)
                   OR (revision_sequence = 1 AND is_value_changed)
                   OR (first_filed_date IS NOT NULL AND latest_filed_date < first_filed_date)
                   OR (first_available_at IS NOT NULL AND latest_available_at < first_available_at)
            """,
            threshold=0.0,
            required_tables=("fundamental_fact_revisions",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="sec_company_facts_without_revision_rows",
            sql="""
                WITH fact_keys AS (
                    SELECT
                        sha256(
                            concat_ws(
                                '|',
                                source,
                                security_id,
                                taxonomy,
                                concept,
                                unit,
                                coalesce(CAST(period_start AS VARCHAR), ''),
                                CAST(period_end AS VARCHAR),
                                accession_number
                            )
                        ) AS fact_revision_id
                    FROM sec_company_facts
                )
                SELECT count(*)::DOUBLE
                FROM fact_keys f
                LEFT JOIN fundamental_fact_revisions r
                  ON r.fact_revision_id = f.fact_revision_id
                WHERE r.fact_revision_id IS NULL
            """,
            threshold=0.0,
            required_tables=("sec_company_facts", "fundamental_fact_revisions"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="revision_rows_without_sec_company_facts",
            sql="""
                WITH fact_keys AS (
                    SELECT
                        sha256(
                            concat_ws(
                                '|',
                                source,
                                security_id,
                                taxonomy,
                                concept,
                                unit,
                                coalesce(CAST(period_start AS VARCHAR), ''),
                                CAST(period_end AS VARCHAR),
                                accession_number
                            )
                        ) AS fact_revision_id
                    FROM sec_company_facts
                )
                SELECT count(*)::DOUBLE
                FROM fundamental_fact_revisions r
                LEFT JOIN fact_keys f
                  ON f.fact_revision_id = r.fact_revision_id
                WHERE f.fact_revision_id IS NULL
            """,
            threshold=0.0,
            required_tables=("sec_company_facts", "fundamental_fact_revisions"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="bad_latest_fundamental_fact_revisions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        revision_group_id,
                        sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows
                    FROM fundamental_fact_revisions
                    GROUP BY 1
                    HAVING latest_rows <> 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_fact_revisions",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="bad_fundamental_revision_change_flags",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_fact_revisions
                WHERE is_value_changed <> CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE (value IS DISTINCT FROM previous_value)
                END
            """,
            threshold=0.0,
            required_tables=("fundamental_fact_revisions",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="duplicate_fundamental_statement_map_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, taxonomy, concept, industry_template, count(*) AS row_count
                    FROM fundamental_statement_map
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_map",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="bad_fundamental_statement_map_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_map
                WHERE source IS NULL
                   OR source = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR statement_type NOT IN ('balance_sheet', 'income_statement', 'cash_flow', 'per_share', 'share_count', 'bank_statement', 'insurance_statement', 'reit_statement', 'utility_statement', 'broker_dealer_statement', 'other')
                   OR statement_section IS NULL
                   OR statement_section = ''
                   OR canonical_metric IS NULL
                   OR canonical_metric = ''
                   OR canonical_label IS NULL
                   OR canonical_label = ''
                   OR period_type NOT IN ('instant', 'duration')
                   OR normal_balance NOT IN ('debit', 'credit', 'not_applicable')
                   OR unit_type NOT IN ('monetary', 'shares', 'per_share', 'ratio', 'count', 'quantity', 'other')
                   OR value_multiplier IS NULL
                   OR concept_priority < 1
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_map",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="loaded_xbrl_concepts_without_statement_map",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_concept_catalog c
                LEFT JOIN fundamental_statement_map m
                  ON m.source = c.source
                 AND m.taxonomy = c.taxonomy
                 AND m.concept = c.concept
                 AND m.is_active
                WHERE m.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_concept_catalog", "fundamental_statement_map"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_concept_coverage_report",
            table_name="fundamental_unmapped_concept_report",
            check_name="fundamental_unmapped_concept_report_empty",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_unmapped_concept_report
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_unmapped_concept_report",),
            detail_sql="""
                SELECT *
                FROM fundamental_unmapped_concept_report
                ORDER BY gap_type, coalesce(item_id, 999999999), taxonomy, concept
                LIMIT 25
            """,
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="duplicate_fundamental_statement_point_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT statement_point_id, count(*) AS row_count
                    FROM fundamental_statement_points
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="bad_fundamental_statement_point_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points
                WHERE statement_point_id IS NULL
                   OR statement_point_id = ''
                   OR fact_revision_id IS NULL
                   OR fact_revision_id = ''
                   OR revision_group_id IS NULL
                   OR revision_group_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR statement_type NOT IN ('balance_sheet', 'income_statement', 'cash_flow', 'per_share', 'share_count', 'bank_statement', 'insurance_statement', 'reit_statement', 'utility_statement', 'broker_dealer_statement', 'other')
                   OR statement_section IS NULL
                   OR statement_section = ''
                   OR canonical_metric IS NULL
                   OR canonical_metric = ''
                   OR canonical_label IS NULL
                   OR canonical_label = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR unit_type NOT IN ('monetary', 'shares', 'per_share', 'ratio', 'count', 'other')
                   OR period_type NOT IN ('instant', 'duration')
                   OR normal_balance NOT IN ('debit', 'credit', 'not_applicable')
                   OR period_end IS NULL
                   OR as_of_date IS NULL
                   OR period_end > as_of_date
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR (period_type = 'duration' AND period_start IS NULL)
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="statement_points_without_map",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points p
                LEFT JOIN fundamental_statement_map m
                  ON m.source = p.source
                 AND m.taxonomy = p.taxonomy
                 AND m.concept = p.concept
                 AND m.is_active
                WHERE m.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points", "fundamental_statement_map"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="statement_points_without_revision_row",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points p
                LEFT JOIN fundamental_fact_revisions r
                  ON r.fact_revision_id = p.fact_revision_id
                WHERE r.fact_revision_id IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points", "fundamental_fact_revisions"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="mapped_statement_concepts_without_points",
            sql="""
                WITH mapped_concepts AS (
                    SELECT DISTINCT m.source, m.taxonomy, m.concept
                    FROM fundamental_statement_map m
                    JOIN fundamental_fact_revisions r
                      ON r.source = m.source
                     AND r.taxonomy = m.taxonomy
                     AND r.concept = m.concept
                    WHERE m.is_active
                )
                SELECT count(*)::DOUBLE
                FROM mapped_concepts m
                LEFT JOIN fundamental_statement_points p
                  ON p.source = m.source
                 AND p.taxonomy = m.taxonomy
                 AND p.concept = m.concept
                WHERE p.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_map", "fundamental_fact_revisions", "fundamental_statement_points"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="bad_latest_fundamental_statement_points",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        canonical_metric,
                        unit,
                        period_start,
                        period_end,
                        sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows
                    FROM fundamental_statement_points
                    GROUP BY 1, 2, 3, 4, 5, 6
                    HAVING latest_rows <> 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="bad_fundamental_statement_value_mapping",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points p
                JOIN fundamental_statement_map m
                  ON m.source = p.source
                 AND m.taxonomy = p.taxonomy
                 AND m.concept = p.concept
                WHERE p.value IS DISTINCT FROM p.raw_value * m.value_multiplier
                   OR p.previous_value IS DISTINCT FROM p.previous_raw_value * m.value_multiplier
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points", "fundamental_statement_map"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_item_alias",
            table_name="fundamental_item_alias",
            check_name="duplicate_fundamental_item_alias_item_mappings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT alias_scheme, alias_code
                    FROM fundamental_item_alias
                    WHERE alias_scheme IS NOT NULL
                      AND alias_scheme <> ''
                      AND alias_code IS NOT NULL
                      AND alias_code <> ''
                    GROUP BY 1, 2
                    HAVING count(DISTINCT item_id) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_item_alias",),
            detail_sql="""
                SELECT
                    alias_scheme AS taxonomy,
                    alias_code AS concept,
                    count(DISTINCT item_id) AS item_id_count,
                    min(item_id) AS min_item_id,
                    max(item_id) AS max_item_id
                FROM fundamental_item_alias
                WHERE alias_scheme IS NOT NULL
                  AND alias_scheme <> ''
                  AND alias_code IS NOT NULL
                  AND alias_code <> ''
                GROUP BY 1, 2
                HAVING count(DISTINCT item_id) > 1
                ORDER BY taxonomy, concept
                LIMIT 50
            """,
        ),
        SqlQualityCheck(
            dataset_id="fundamental_item_alias",
            table_name="fundamental_item_alias",
            check_name="unmapped_fundamental_fact_concepts",
            sql="""
                WITH fact_concepts AS (
                    SELECT 'fundamental_statement_points' AS fact_table, taxonomy, concept
                    FROM fundamental_statement_points
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND concept IS NOT NULL
                      AND concept <> ''
                    UNION ALL
                    SELECT 'fundamental_points' AS fact_table, taxonomy, metric AS concept
                    FROM fundamental_points
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND metric IS NOT NULL
                      AND metric <> ''
                ),
                alias_counts AS (
                    SELECT alias_scheme, alias_code, count(DISTINCT item_id) AS item_id_count
                    FROM fundamental_item_alias
                    WHERE alias_scheme IS NOT NULL
                      AND alias_scheme <> ''
                      AND alias_code IS NOT NULL
                      AND alias_code <> ''
                    GROUP BY 1, 2
                )
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT f.fact_table, f.taxonomy, f.concept
                    FROM fact_concepts f
                    LEFT JOIN alias_counts a
                      ON a.alias_scheme = f.taxonomy
                     AND a.alias_code = f.concept
                    WHERE coalesce(a.item_id_count, 0) = 0
                )
            """,
            threshold=0.0,
            required_tables=(
                "fundamental_statement_points",
                "fundamental_points",
                "fundamental_item_alias",
            ),
            failure_status="warning",
            detail_sql="""
                WITH fact_concepts AS (
                    SELECT 'fundamental_statement_points' AS fact_table, taxonomy, concept
                    FROM fundamental_statement_points
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND concept IS NOT NULL
                      AND concept <> ''
                    UNION ALL
                    SELECT 'fundamental_points' AS fact_table, taxonomy, metric AS concept
                    FROM fundamental_points
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND metric IS NOT NULL
                      AND metric <> ''
                ),
                alias_counts AS (
                    SELECT alias_scheme, alias_code, count(DISTINCT item_id) AS item_id_count
                    FROM fundamental_item_alias
                    WHERE alias_scheme IS NOT NULL
                      AND alias_scheme <> ''
                      AND alias_code IS NOT NULL
                      AND alias_code <> ''
                    GROUP BY 1, 2
                )
                SELECT DISTINCT f.fact_table, f.taxonomy, f.concept
                FROM fact_concepts f
                LEFT JOIN alias_counts a
                  ON a.alias_scheme = f.taxonomy
                 AND a.alias_code = f.concept
                WHERE coalesce(a.item_id_count, 0) = 0
                ORDER BY f.fact_table, f.taxonomy, f.concept
                LIMIT 50
            """,
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="duplicate_fundamental_ttm_point_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT ttm_point_id, count(*) AS row_count
                    FROM fundamental_ttm_points
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="bad_fundamental_ttm_point_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ttm_points
                WHERE ttm_point_id IS NULL
                   OR ttm_point_id = ''
                   OR ttm_revision_group_id IS NULL
                   OR ttm_revision_group_id = ''
                   OR anchor_statement_point_id IS NULL
                   OR anchor_statement_point_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR statement_type NOT IN ('income_statement', 'cash_flow', 'per_share')
                   OR statement_section IS NULL
                   OR statement_section = ''
                   OR canonical_metric IS NULL
                   OR canonical_metric = ''
                   OR canonical_label IS NULL
                   OR canonical_label = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR unit_type NOT IN ('monetary', 'per_share')
                   OR ttm_start_date IS NULL
                   OR ttm_end_date IS NULL
                   OR ttm_start_date > ttm_end_date
                   OR as_of_date IS NULL
                   OR ttm_end_date > as_of_date
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR quarter_count <> 4
                   OR coverage_days NOT BETWEEN 330 AND 380
                   OR min_input_available_at IS NULL
                   OR max_input_available_at IS NULL
                   OR max_input_available_at < min_input_available_at
                   OR (available_at IS NOT NULL AND available_at < max_input_available_at)
                   OR input_statement_point_ids_json IS NULL
                   OR input_statement_point_ids_json IN ('', '[]')
                   OR input_accessions_json IS NULL
                   OR input_accessions_json IN ('', '[]')
                   OR input_period_ends_json IS NULL
                   OR input_period_ends_json IN ('', '[]')
                   OR ttm_value IS NULL
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR (revision_sequence = 1 AND is_value_changed)
                   OR calculation_method NOT IN (
                       'sum_four_visible_quarter_like_statement_points_with_ytd_quarter_derivations',
                       'stitched_quarterly_ttm'
                   )
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="ttm_points_without_anchor_statement_point",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ttm_points t
                LEFT JOIN fundamental_statement_points p
                  ON p.statement_point_id = t.anchor_statement_point_id
                WHERE p.statement_point_id IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points", "fundamental_statement_points"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="bad_latest_fundamental_ttm_points",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        ttm_revision_group_id,
                        sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows
                    FROM fundamental_ttm_points
                    GROUP BY 1
                    HAVING latest_rows <> 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="bad_fundamental_ttm_change_flags",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ttm_points
                WHERE is_value_changed <> CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE (ttm_value IS DISTINCT FROM previous_ttm_value)
                END
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points",),
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="fundamental_calendar_map",
            check_name="calendarization_map_exactly_one_label",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        p.fundamental_period_id,
                        count(m.calendar_map_id) AS map_rows,
                        sum(CASE
                            WHEN m.fiscal_scheme_period IS NULL
                              OR m.fiscal_scheme_period = ''
                              OR m.containing_calendar_period IS NULL
                              OR m.containing_calendar_period = ''
                              OR m.greatest_overlap_calendar_period IS NULL
                              OR m.greatest_overlap_calendar_period = ''
                            THEN 1 ELSE 0
                        END) AS bad_label_rows
                    FROM fundamental_periods p
                    LEFT JOIN fundamental_calendar_map m
                      ON m.fundamental_period_id = p.fundamental_period_id
                     AND m.is_latest_revision
                    GROUP BY p.fundamental_period_id
                    HAVING map_rows <> 1
                        OR bad_label_rows <> 0
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_periods", "fundamental_calendar_map"),
            detail_sql="""
                SELECT
                    p.fundamental_period_id,
                    count(m.calendar_map_id) AS map_rows,
                    sum(CASE
                        WHEN m.fiscal_scheme_period IS NULL
                          OR m.fiscal_scheme_period = ''
                          OR m.containing_calendar_period IS NULL
                          OR m.containing_calendar_period = ''
                          OR m.greatest_overlap_calendar_period IS NULL
                          OR m.greatest_overlap_calendar_period = ''
                        THEN 1 ELSE 0
                    END) AS bad_label_rows
                FROM fundamental_periods p
                LEFT JOIN fundamental_calendar_map m
                  ON m.fundamental_period_id = p.fundamental_period_id
                 AND m.is_latest_revision
                GROUP BY p.fundamental_period_id
                HAVING map_rows <> 1
                    OR bad_label_rows <> 0
                ORDER BY p.fundamental_period_id
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="fundamental_calendar_map",
            check_name="calendarization_53_week_flagged",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_calendar_map
                WHERE (
                       period_length_days >= 371
                    OR (normalized_period_type = 'annual' AND period_length_days > 364)
                    OR (normalized_period_type = 'quarter' AND period_length_days > 91)
                )
                  AND NOT is_53_week
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_calendar_map",),
            detail_sql="""
                SELECT
                    fundamental_period_id,
                    security_id,
                    period_start,
                    period_end,
                    normalized_period_type,
                    period_length_days,
                    week_count,
                    is_53_week
                FROM fundamental_calendar_map
                WHERE (
                       period_length_days >= 371
                    OR (normalized_period_type = 'annual' AND period_length_days > 364)
                    OR (normalized_period_type = 'quarter' AND period_length_days > 91)
                )
                  AND NOT is_53_week
                ORDER BY security_id, period_end
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="fundamental_calendar_ttm",
            check_name="calendar_ttm_no_duplicate_windows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        upstream_source,
                        security_id,
                        calendar_period,
                        canonical_metric,
                        unit,
                        revision_sequence,
                        count(*) AS row_count
                    FROM fundamental_calendar_ttm
                    GROUP BY 1, 2, 3, 4, 5, 6, 7
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_calendar_ttm",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="fundamental_ttm_points",
            check_name="stitched_ttm_no_duplicate_windows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        ttm_end_date,
                        canonical_metric,
                        unit,
                        revision_sequence,
                        count(*) AS row_count
                    FROM fundamental_ttm_points
                    WHERE calculation_method = 'stitched_quarterly_ttm'
                    GROUP BY 1, 2, 3, 4, 5, 6
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ttm_points",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="calendarization_coverage",
            check_name="calendarization_coverage_green",
            sql="""
                SELECT coalesce(sum(
                    fiscal_scheme_unmapped_count
                    + containing_scheme_unmapped_count
                    + overlap_scheme_unmapped_count
                    + duplicate_map_count
                    + unflagged_53_week_count
                    + duplicate_calendar_ttm_window_count
                    + duplicate_stitched_ttm_window_count
                ), 0)::DOUBLE
                FROM calendarization_coverage
                WHERE is_latest_revision
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("calendarization_coverage",),
            detail_sql="""
                SELECT *
                FROM calendarization_coverage
                WHERE is_latest_revision
                  AND (
                    fiscal_scheme_unmapped_count
                    + containing_scheme_unmapped_count
                    + overlap_scheme_unmapped_count
                    + duplicate_map_count
                    + unflagged_53_week_count
                    + duplicate_calendar_ttm_window_count
                    + duplicate_stitched_ttm_window_count
                  ) <> 0
                ORDER BY available_at DESC
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="segments",
            table_name="segment_fact",
            check_name="bad_segment_fact_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM segment_fact
                WHERE segment_fact_id IS NULL OR segment_fact_id = ''
                   OR segment_dim_id IS NULL OR segment_dim_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR canonical_item IS NULL OR canonical_item = ''
                   OR consolidated_code IS NULL OR consolidated_code = ''
                   OR value IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR source_loaded_at IS NULL
                   OR reconciliation_status NOT IN ('reconciled', 'flagged_divergent', 'no_consolidated')
                   OR reconciliation_tolerance IS NULL
                   OR revision_sequence < 1
                   OR revision_count < revision_sequence
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("segment_fact",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="segments",
            table_name="segment_fact",
            check_name="duplicate_segment_fact_revision_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        segment_dim_id,
                        canonical_item,
                        coalesce(CAST(period_start AS VARCHAR), ''),
                        as_of_date,
                        revision_sequence,
                        count(*) AS row_count
                    FROM segment_fact
                    GROUP BY 1, 2, 3, 4, 5, 6, 7
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("segment_fact",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="segments",
            table_name="segment_fact",
            check_name="segment_reconciliation_divergence_warning",
            sql="""
                SELECT count(*)::DOUBLE
                FROM segment_fact
                WHERE is_latest_revision
                  AND reconciliation_status = 'flagged_divergent'
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("segment_fact",),
            failure_status="warning",
            detail_sql="""
                SELECT
                    security_id,
                    canonical_item,
                    as_of_date,
                    segment_sum_value,
                    consolidated_value,
                    reconciliation_difference,
                    reconciliation_relative_difference,
                    reconciliation_tolerance
                FROM segment_fact
                WHERE is_latest_revision
                  AND reconciliation_status = 'flagged_divergent'
                ORDER BY abs(coalesce(reconciliation_relative_difference, 0)) DESC,
                         security_id,
                         canonical_item
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="footnotes",
            table_name="footnote_pension",
            check_name="bad_footnote_subledger_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT pension_id AS row_id, source, security_id, line_item, value, as_of_date, available_at, source_loaded_at, revision_sequence, revision_count
                    FROM footnote_pension
                    UNION ALL
                    SELECT deferred_tax_id AS row_id, source, security_id, line_item, value, as_of_date, available_at, source_loaded_at, revision_sequence, revision_count
                    FROM footnote_deferred_tax
                    UNION ALL
                    SELECT lease_id AS row_id, source, security_id, line_item, value, as_of_date, available_at, source_loaded_at, revision_sequence, revision_count
                    FROM footnote_lease
                    UNION ALL
                    SELECT sbc_id AS row_id, source, security_id, line_item, value, as_of_date, available_at, source_loaded_at, revision_sequence, revision_count
                    FROM footnote_sbc
                )
                WHERE row_id IS NULL OR row_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR line_item IS NULL OR line_item = ''
                   OR value IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR source_loaded_at IS NULL
                   OR revision_sequence < 1
                   OR revision_count < revision_sequence
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=(
                "footnote_pension",
                "footnote_deferred_tax",
                "footnote_lease",
                "footnote_sbc",
            ),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="segment_footnote_coverage",
            table_name="segment_footnote_coverage",
            check_name="segment_footnote_coverage_counts_valid",
            sql="""
                SELECT count(*)::DOUBLE
                FROM segment_footnote_coverage
                WHERE segment_dim_count < 0
                   OR segment_fact_count < 0
                   OR segment_reconciled_count < 0
                   OR segment_flagged_divergent_count < 0
                   OR segment_no_consolidated_count < 0
                   OR footnote_pension_count < 0
                   OR footnote_deferred_tax_count < 0
                   OR footnote_lease_count < 0
                   OR footnote_sbc_count < 0
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("segment_footnote_coverage",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_periods",
            table_name="fundamental_periods",
            check_name="duplicate_fundamental_period_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        period_start,
                        period_end,
                        accession_number,
                        count(*) AS row_count
                    FROM fundamental_periods
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_periods",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_periods",
            table_name="fundamental_periods",
            check_name="bad_fundamental_period_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_periods
                WHERE fundamental_period_id IS NULL
                   OR fundamental_period_id = ''
                   OR period_group_id IS NULL
                   OR period_group_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR period_end IS NULL
                   OR datadate IS NULL
                   OR datadate <> period_end
                   OR as_of_date IS NULL
                   OR fdate IS NULL
                   OR fdate <> as_of_date
                   OR ldate IS NULL
                   OR ldate < fdate
                   OR (rdq IS NOT NULL AND rdq < datadate)
                   OR (rdq IS NOT NULL AND rdq > fdate)
                   OR (pdate IS NOT NULL AND pdate IS DISTINCT FROM rdq)
                   OR period_end > as_of_date
                   OR available_at IS NULL
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR normalized_period_type NOT IN (
                       'instant',
                       'quarter',
                       'semiannual_ytd',
                       'multi_quarter_ytd',
                       'annual',
                       'multi_year_comparative',
                       'other'
                   )
                   OR (period_start IS NOT NULL AND period_start > period_end)
                   OR (period_start IS NULL AND period_days IS NOT NULL)
                   OR (period_start IS NOT NULL AND period_days <> date_diff('day', period_start, period_end) + 1)
                   OR calendar_year IS NULL
                   OR calendar_quarter NOT BETWEEN 1 AND 4
                   OR calendar_period IS NULL
                   OR calendar_period = ''
                   OR reported_fiscal_years_json IS NULL
                   OR reported_fiscal_periods_json IS NULL
                   OR statement_types_json IS NULL
                   OR statement_types_json IN ('', '[]')
                   OR canonical_metrics_json IS NULL
                   OR canonical_metrics_json IN ('', '[]')
                   OR input_statement_point_ids_json IS NULL
                   OR input_statement_point_ids_json IN ('', '[]')
                   OR statement_point_count <= 0
                   OR canonical_metric_count <= 0
                   OR concept_count <= 0
                   OR value_changed_statement_count < 0
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR (first_available_at IS NOT NULL AND latest_available_at < first_available_at)
            """,
            threshold=0.0,
            required_tables=("fundamental_periods",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_periods",
            table_name="fundamental_periods",
            check_name="statement_points_without_fundamental_period",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points p
                LEFT JOIN fundamental_periods fp
                  ON fp.source = p.source
                 AND fp.security_id = p.security_id
                 AND fp.period_end = p.period_end
                 AND fp.accession_number = p.accession_number
                 AND (
                     fp.period_start IS NOT DISTINCT FROM p.period_start
                 )
                WHERE fp.fundamental_period_id IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points", "fundamental_periods"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_periods",
            table_name="fundamental_periods",
            check_name="bad_latest_fundamental_periods",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        period_group_id,
                        sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows
                    FROM fundamental_periods
                    GROUP BY 1
                    HAVING latest_rows <> 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_periods",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_reconciliation",
            table_name="fundamental_reconciliation_serving",
            check_name="duplicate_fundamental_reconciliation_events",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT reconciliation_group_id,available_at,count(*) AS row_count
                    FROM fundamental_reconciliation_serving
                    GROUP BY 1,2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_reconciliation_serving",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_reconciliation",
            table_name="fundamental_reconciliation_serving",
            check_name="bad_fundamental_reconciliation_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_reconciliation_serving
                WHERE reconciliation_id IS NULL OR reconciliation_id = ''
                   OR reconciliation_group_id IS NULL OR reconciliation_group_id = ''
                   OR rule_id IS NULL OR rule_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR basis NOT IN ('annual','quarterly','ttm','instant')
                   OR period_end IS NULL
                   OR status NOT IN (
                        'reconciled','mismatch','diagnostic_difference','not_applicable'
                   )
                   OR mismatch_severity NOT IN ('error','diagnostic')
                   OR is_applicable <> (status <> 'not_applicable')
                   OR (status = 'diagnostic_difference' AND mismatch_severity <> 'diagnostic')
                   OR (status = 'mismatch' AND mismatch_severity <> 'error')
                   OR input_filing_status NOT IN (
                        'single_filing','mixed_filing_vintage','unknown_accession'
                   )
                   OR input_accession_count < 0
                   OR input_accessions_json IS NULL
                   OR input_accessions_json = ''
                   OR context_verification_status NOT IN (
                        'verified_same_context','verified_same_context_with_extension_map',
                        'context_not_loaded',
                        'context_not_aligned','mixed_filing_vintage','unknown_accession'
                   )
                   OR context_evidence_json IS NULL
                   OR context_evidence_json = ''
                   OR mismatch_reason NOT IN (
                        'within_tolerance','rule_not_applicable','diagnostic_rule_difference',
                        'mixed_filing_vintage','context_not_loaded',
                        'xbrl_context_not_aligned','verified_accounting_mismatch',
                        'within_tolerance_extension_mapped','unknown_accession'
                   )
                   OR extension_mapping_applied <> (
                        context_verification_status='verified_same_context_with_extension_map'
                   )
                   OR extension_inputs_json IS NULL OR extension_inputs_json=''
                   OR (extension_mapping_applied AND extension_inputs_json='[]')
                   OR (NOT extension_mapping_applied AND extension_inputs_json<>'[]')
                   OR (is_hard_failure AND (
                        status <> 'mismatch'
                        OR mismatch_severity <> 'error'
                        OR context_verification_status <> 'verified_same_context'
                   ))
                   OR (context_verification_status IN (
                           'verified_same_context','verified_same_context_with_extension_map'
                       )
                       AND input_filing_status <> 'single_filing')
                   OR tolerance < 0
                   OR input_standardized_ids_json IS NULL
                   OR input_standardized_ids_json IN ('','[]')
                   OR input_item_ids_json IS NULL
                   OR input_item_ids_json IN ('','[]')
                   OR input_values_json IS NULL
                   OR input_values_json IN ('','[]')
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR update_type NOT IN (
                        'original','restated','classification_update','metadata_update'
                   )
                   OR (revision_sequence = 1 AND update_type <> 'original')
            """,
            threshold=0.0,
            required_tables=("fundamental_reconciliation_serving",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_reconciliation",
            table_name="fundamental_reconciliation_serving",
            check_name="bad_latest_fundamental_reconciliation_chains",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        reconciliation_group_id,
                        sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows
                    FROM fundamental_reconciliation_serving
                    GROUP BY 1
                    HAVING latest_rows <> 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_reconciliation_serving",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_reconciliation",
            table_name="fundamental_reconciliation_serving",
            check_name="latest_hard_fundamental_reconciliation_mismatch_rate",
            sql="""
                SELECT coalesce(
                    count(*) FILTER (
                        WHERE is_latest_revision AND is_hard_failure
                    )::DOUBLE
                    / nullif(count(*) FILTER (
                        WHERE is_latest_revision AND is_applicable
                          AND mismatch_severity='error'
                          AND context_verification_status='verified_same_context'
                    ),0),
                    0.0
                )
                FROM fundamental_reconciliation_serving
            """,
            threshold=0.05,
            comparator="le",
            required_tables=("fundamental_reconciliation_serving",),
            failure_status="warning",
            severity="warning",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_reconciliation",
            table_name="fundamental_reconciliation_serving",
            check_name="fundamental_reconciliation_context_verification_rate",
            sql="""
                SELECT coalesce(
                    count(*) FILTER (
                        WHERE is_latest_revision AND is_applicable
                          AND mismatch_severity='error'
                          AND context_verification_status IN (
                              'verified_same_context',
                              'verified_same_context_with_extension_map'
                          )
                    )::DOUBLE
                    / nullif(count(*) FILTER (
                        WHERE is_latest_revision AND is_applicable
                          AND mismatch_severity='error'
                    ),0),
                    1.0
                )
                FROM fundamental_reconciliation_serving
            """,
            threshold=0.80,
            comparator="ge",
            required_tables=("fundamental_reconciliation_serving",),
            failure_status="warning",
            severity="warning",
        ),
    )
