from __future__ import annotations

from ._check_common import DEFAULT_EXPORT_OBJECTS, _export_scan_internal_cusip_sql
from ._types import SqlQualityCheck


def estimate_check_specs(
    *,
    daily_macro_stale_days: int,
    monthly_macro_stale_days: int,
    valuation_stale_gap_days: int,
) -> tuple[SqlQualityCheck, ...]:
    return (
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_null_value",
            sql="SELECT count(*)::DOUBLE FROM est_actual WHERE value IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_invalid_fiscal_period",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_actual
                WHERE fiscal_period NOT IN ('Q1','Q2','Q3','Q4','FY')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_actual WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_eps_missing_basis",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_actual
                WHERE measure_code LIKE 'EPS%'
                  AND (basis IS NULL OR basis = '')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_duplicate_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, measure_code, fiscal_year, fiscal_period,
                           accession_number, count(*) AS row_count
                    FROM est_actual
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_guidance",
            table_name="est_guidance",
            check_name="est_guidance_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_guidance WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_guidance",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_guidance",
            table_name="est_guidance",
            check_name="est_guidance_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_guidance_id, count(*) AS row_count
                    FROM est_guidance
                    WHERE est_guidance_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_guidance",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_guidance",
            table_name="est_guidance",
            check_name="est_guidance_invalid_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_guidance
                WHERE period_end IS NULL
                   OR security_id IS NULL
                   OR measure_code IS NULL
                   OR (low IS NULL AND high IS NULL AND mid IS NULL)
                   OR (low IS NOT NULL AND high IS NOT NULL AND high < low)
                   OR (
                       extraction_confidence IS NOT NULL
                       AND (extraction_confidence < 0 OR extraction_confidence > 1)
                   )
                   OR (
                       guidance_type IS NOT NULL
                       AND guidance_type NOT IN ('POINT', 'RANGE', 'OPEN_LOW', 'OPEN_HIGH', 'QUAL')
                   )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_guidance",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_consensus WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_consensus_id, count(*) AS row_count
                    FROM est_consensus
                    WHERE est_consensus_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_invalid_stat_range",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_consensus
                WHERE (high IS NOT NULL AND low IS NOT NULL AND high < low)
                   OR (mean IS NOT NULL AND high IS NOT NULL AND mean > high)
                   OR (mean IS NOT NULL AND low IS NOT NULL AND mean < low)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_negative_counts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_consensus
                WHERE coalesce(num_estimates, 0) < 0
                   OR coalesce(num_up, 0) < 0
                   OR coalesce(num_down, 0) < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_invalid_stale_window",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_consensus
                WHERE stale_after_date IS NOT NULL
                  AND consensus_date IS NOT NULL
                  AND stale_after_date < consensus_date
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_recommendation WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_recommendation_id, count(*) AS row_count
                    FROM est_recommendation
                    WHERE est_recommendation_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_invalid_code",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation
                WHERE recommendation_code IS NOT NULL
                  AND recommendation_code NOT BETWEEN 1 AND 5
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_invalid_price_target",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation
                WHERE (price_target IS NOT NULL AND price_target <= 0)
                   OR (target_horizon_months IS NOT NULL AND target_horizon_months <= 0)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_invalid_active_window",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation
                WHERE rating_date IS NOT NULL
                  AND (
                        (revision_date IS NOT NULL AND revision_date < rating_date)
                     OR (stop_date IS NOT NULL AND stop_date < rating_date)
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_recommendation_summary WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_recommendation_summary_id, count(*) AS row_count
                    FROM est_recommendation_summary
                    WHERE est_recommendation_summary_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_invalid_rating_mean",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation_summary
                WHERE (mean_recommendation IS NOT NULL AND (mean_recommendation < 1 OR mean_recommendation > 5))
                   OR (median_recommendation IS NOT NULL AND (median_recommendation < 1 OR median_recommendation > 5))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_negative_counts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation_summary
                WHERE coalesce(strong_buy_count, 0) < 0
                   OR coalesce(buy_count, 0) < 0
                   OR coalesce(hold_count, 0) < 0
                   OR coalesce(underperform_count, 0) < 0
                   OR coalesce(sell_count, 0) < 0
                   OR coalesce(buy_equivalent_count, 0) < 0
                   OR coalesce(sell_equivalent_count, 0) < 0
                   OR coalesce(total_recommendations, 0) < 0
                   OR coalesce(price_target_count, 0) < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_bad_total_count",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation_summary
                WHERE total_recommendations IS NOT NULL
                  AND (
                    coalesce(strong_buy_count, 0)
                    + coalesce(buy_count, 0)
                    + coalesce(hold_count, 0)
                    + coalesce(underperform_count, 0)
                    + coalesce(sell_count, 0)
                  ) > total_recommendations
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_invalid_price_target_stats",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation_summary
                WHERE coalesce(mean_price_target, 1) <= 0
                   OR coalesce(median_price_target, 1) <= 0
                   OR coalesce(high_price_target, 1) <= 0
                   OR coalesce(low_price_target, 1) <= 0
                   OR (high_price_target IS NOT NULL AND low_price_target IS NOT NULL AND high_price_target < low_price_target)
                   OR (mean_price_target IS NOT NULL AND high_price_target IS NOT NULL AND mean_price_target > high_price_target)
                   OR (mean_price_target IS NOT NULL AND low_price_target IS NOT NULL AND mean_price_target < low_price_target)
                   OR (target_horizon_months IS NOT NULL AND target_horizon_months <= 0)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_security_link WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_security_link_id, count(*) AS row_count
                    FROM est_security_link
                    WHERE est_security_link_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_invalid_status_confidence",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_security_link
                WHERE link_status NOT IN ('accepted', 'conflict')
                   OR confidence < 0
                   OR confidence > 1
                   OR valid_from IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_conflicting_accepted_targets",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT provider, vendor_security_id_type, vendor_security_id
                    FROM est_security_link
                    WHERE link_status = 'accepted'
                    GROUP BY 1, 2, 3
                    HAVING count(DISTINCT target_security_id) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_orphan_target_security",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_security_link l
                LEFT JOIN securities s ON s.security_id = l.target_security_id
                WHERE l.link_status = 'accepted'
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link", "securities"),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_detail",
            table_name="est_detail",
            check_name="est_detail_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_detail WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_detail",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_detail",
            table_name="est_detail",
            check_name="est_detail_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_detail_id, count(*) AS row_count
                    FROM est_detail
                    WHERE est_detail_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_detail",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_detail",
            table_name="est_detail",
            check_name="est_detail_invalid_revision_window",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_detail
                WHERE announce_date IS NOT NULL
                  AND revision_date IS NOT NULL
                  AND revision_date < announce_date
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_detail",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_detail",
            table_name="est_detail",
            check_name="est_detail_invalid_stop_window",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_detail
                WHERE announce_date IS NOT NULL
                  AND stop_date IS NOT NULL
                  AND stop_date < announce_date
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_detail",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_period_dim",
            table_name="est_period_dim",
            check_name="est_period_dim_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_period_id, count(*) AS row_count
                    FROM est_period_dim
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_period_dim",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_surprise",
            table_name="est_surprise",
            check_name="est_surprise_nonfinite_sue",
            # sue must be NULL (insufficient history) or a finite number
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_surprise
                WHERE sue IS NOT NULL
                  AND NOT isfinite(sue)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_surprise",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_surprise",
            table_name="est_surprise",
            check_name="est_surprise_basis_mismatch_pct_null",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_surprise
                WHERE coalesce(basis_mismatch, false)
                  AND surprise_pct IS NOT NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_surprise",),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="press_release_facts",
            table_name="press_release_facts",
            check_name="bad_press_release_fact_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM press_release_facts
                WHERE security_id IS NULL
                   OR measure_code IS NULL
                   OR fiscal_period NOT IN ('Q1','Q2','Q3','Q4','FY')
                   OR period_end IS NULL
                   OR value IS NULL
                   OR available_at IS NULL
                   OR basis IS NULL
                   OR basis = ''
                   OR extraction_confidence IS NULL
                   OR extraction_confidence < 0
                   OR extraction_confidence > 1
                   OR source_item NOT LIKE '%2.02%'
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("press_release_facts",),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="press_release_facts",
            table_name="press_release_facts",
            check_name="press_release_no_lookahead",
            sql="""
                SELECT count(*)::DOUBLE
                FROM press_release_facts
                WHERE release_date IS NOT NULL
                  AND CAST(available_at AS DATE) < release_date
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("press_release_facts",),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="press_release_reconciliation",
            table_name="press_release_reconciliation",
            check_name="press_release_preliminary_vintage_retained",
            sql="""
                SELECT count(*)::DOUBLE
                FROM press_release_reconciliation r
                LEFT JOIN press_release_facts pr
                  ON pr.press_release_fact_id = r.press_release_fact_id
                WHERE pr.press_release_fact_id IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("press_release_reconciliation", "press_release_facts"),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="fundamental_statement_map_concept_coverage",
            sql="""
                SELECT count(DISTINCT item_id)::DOUBLE
                FROM fundamental_statement_map
                WHERE industry_template = 'ALL'
                  AND (
                      item_id BETWEEN 1001 AND 1043
                   OR item_id BETWEEN 1101 AND 1119
                   OR item_id BETWEEN 1201 AND 1223
                   OR item_id BETWEEN 1301 AND 1325
                   OR item_id BETWEEN 1401 AND 1427
                  )
            """,
            threshold=137.0,
            comparator="ge",
            required_tables=("fundamental_statement_map",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="fundamental_statement_map_industry_overlay_coverage",
            sql="""
                SELECT count(DISTINCT item_id)::DOUBLE
                FROM fundamental_statement_map
                WHERE (
                       (industry_template = 'BK' AND item_id BETWEEN 1501 AND 1515)
                    OR (industry_template = 'IS' AND item_id BETWEEN 1601 AND 1610)
                    OR (industry_template = 'RT' AND item_id BETWEEN 1701 AND 1712)
                    OR (industry_template = 'UT' AND item_id BETWEEN 1801 AND 1805)
                    OR (industry_template = 'BD' AND item_id BETWEEN 1901 AND 1905)
                )
            """,
            threshold=47.0,
            comparator="eq",
            required_tables=("fundamental_statement_map",),
        ),
        SqlQualityCheck(
            dataset_id="industry_template",
            table_name="entity_industry_template",
            check_name="industry_template_exactly_one_route",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        s.security_id,
                        count(e.route_id) AS route_count,
                        count(DISTINCT e.industry_template) AS template_count
                    FROM securities s
                    LEFT JOIN entity_industry_template e
                      ON e.security_id = s.security_id
                     AND e.is_latest_revision
                     AND e.valid_from <= current_date
                     AND coalesce(e.valid_to, DATE '9999-12-31') > current_date
                    GROUP BY s.security_id
                    HAVING route_count <> 1
                        OR template_count <> 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("securities", "entity_industry_template"),
            detail_sql="""
                SELECT
                    s.security_id,
                    count(e.route_id) AS route_count,
                    count(DISTINCT e.industry_template) AS template_count,
                    CAST(to_json(list(e.industry_template ORDER BY e.industry_template) FILTER (
                        WHERE e.industry_template IS NOT NULL
                    )) AS VARCHAR) AS templates_json
                FROM securities s
                LEFT JOIN entity_industry_template e
                  ON e.security_id = s.security_id
                 AND e.is_latest_revision
                 AND e.valid_from <= current_date
                 AND coalesce(e.valid_to, DATE '9999-12-31') > current_date
                GROUP BY s.security_id
                HAVING route_count <> 1
                    OR template_count <> 1
                ORDER BY s.security_id
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="industry_template",
            table_name="industry_template_coverage",
            check_name="industry_template_required_item_coverage",
            sql="""
                SELECT coalesce(sum(missing_item_count), 0)::DOUBLE
                FROM industry_template_coverage
                WHERE is_latest_revision
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("industry_template_coverage",),
            detail_sql="""
                SELECT
                    industry_template,
                    routed_entity_count,
                    required_item_count,
                    present_item_count,
                    not_available_item_count,
                    missing_item_count,
                    missing_item_ids_json
                FROM industry_template_coverage
                WHERE is_latest_revision
                  AND missing_item_count <> 0
                ORDER BY industry_template
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="filer_13f_cik_alias",
            table_name="filer_13f_cik_alias",
            check_name="filer_alias_overlapping_authoritative_windows",
            sql="""
                WITH auth AS (
                    SELECT alias_id, alias_cik, valid_from,
                           coalesce(valid_to, DATE '9999-12-31') AS valid_to
                    FROM filer_13f_cik_alias
                    WHERE alias_type IN ('SELF', 'SUBADVISOR', 'MA_CONTINUITY', 'MANUAL')
                )
                SELECT count(*)::DOUBLE
                FROM auth a
                JOIN auth b
                  ON a.alias_cik = b.alias_cik
                 AND a.alias_id <> b.alias_id
                 AND a.valid_from < b.valid_to
                 AND b.valid_from < a.valid_to
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("filer_13f_cik_alias",),
        ),
        SqlQualityCheck(
            dataset_id="filer_13f_cik_alias",
            table_name="filer_13f_cik_alias",
            check_name="filer_alias_confidence_out_of_range",
            sql="""
                SELECT count(*)::DOUBLE
                FROM filer_13f_cik_alias
                WHERE confidence < 0.0 OR confidence > 1.0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("filer_13f_cik_alias",),
        ),
        SqlQualityCheck(
            dataset_id="filer_13f_cik_alias",
            table_name="filer_13f_cik_alias",
            check_name="filer_alias_candidate_primary_has_self",
            sql="""
                SELECT count(*)::DOUBLE
                FROM filer_13f_cik_alias c
                WHERE c.alias_type = 'NAME_MATCH_CANDIDATE'
                  AND NOT EXISTS (
                      SELECT 1 FROM filer_13f_cik_alias s
                      WHERE s.alias_type = 'SELF' AND s.alias_cik = c.primary_cik
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("filer_13f_cik_alias",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_volume",
            table_name="offexchange_volume",
            check_name="offexchange_volume_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT symbol, mpid, venue_class, period_type, summary_start_date,
                           count(*) FILTER (WHERE is_latest) AS latest_rows
                    FROM offexchange_volume
                    GROUP BY symbol, mpid, venue_class, period_type, summary_start_date
                    HAVING count(*) FILTER (WHERE is_latest) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_volume",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_volume",
            table_name="offexchange_volume",
            check_name="offexchange_volume_bad_venue_class",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_volume
                WHERE venue_class NOT IN ('ATS', 'non_ATS')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_volume",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_security_period",
            table_name="offexchange_security_period",
            check_name="offexchange_security_period_pct_out_of_range",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_security_period
                WHERE ats_share_pct IS NOT NULL
                  AND (ats_share_pct < 0.0 OR ats_share_pct > 100.0)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_security_period",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_security_period",
            table_name="offexchange_security_period",
            check_name="offexchange_security_period_total_inconsistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_security_period
                WHERE abs(coalesce(total_share_quantity, 0)
                          - (coalesce(ats_share_quantity, 0) + coalesce(non_ats_share_quantity, 0))) > 1.0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_security_period",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_volume",
            table_name="finra_short_volume",
            check_name="finra_short_volume_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, symbol, trade_date, market_code,
                           count(*) FILTER (WHERE is_latest) AS latest_rows
                    FROM finra_short_volume
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) FILTER (WHERE is_latest) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("finra_short_volume",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_volume",
            table_name="finra_short_volume",
            check_name="finra_short_volume_bad_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM finra_short_volume
                WHERE symbol IS NULL
                   OR symbol = ''
                   OR trade_date IS NULL
                   OR market_code IS NULL
                   OR market_code = ''
                   OR available_at IS NULL
                   OR coalesce(short_volume, -1) < 0
                   OR coalesce(short_exempt_volume, -1) < 0
                   OR coalesce(total_volume, -1) < 0
                   OR short_volume > total_volume
                   OR short_exempt_volume > total_volume
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("finra_short_volume",),
        ),
        SqlQualityCheck(
            dataset_id="short_volume_metrics",
            table_name="short_volume_metrics",
            check_name="short_volume_metrics_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, symbol, trade_date,
                           count(*) FILTER (WHERE is_latest_revision) AS latest_rows
                    FROM short_volume_metrics
                    GROUP BY 1, 2, 3
                    HAVING count(*) FILTER (WHERE is_latest_revision) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_volume_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_volume_metrics",
            table_name="short_volume_metrics",
            check_name="short_volume_metrics_bad_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_volume_metrics
                WHERE symbol IS NULL
                   OR symbol = ''
                   OR trade_date IS NULL
                   OR available_at IS NULL
                   OR coalesce(short_volume, -1) < 0
                   OR coalesce(short_exempt_volume, -1) < 0
                   OR coalesce(total_volume, -1) < 0
                   OR short_volume > total_volume
                   OR short_exempt_volume > total_volume
                   OR short_volume_ratio < 0 OR short_volume_ratio > 1
                   OR short_exempt_ratio < 0 OR short_exempt_ratio > 1
                   OR short_volume_ratio_percentile < 0 OR short_volume_ratio_percentile > 1
                   OR short_exempt_ratio_percentile < 0 OR short_exempt_ratio_percentile > 1
                   OR dominant_market_share_pct < 0 OR dominant_market_share_pct > 100
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_volume_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_volume_metrics",
            table_name="short_volume_metrics",
            check_name="short_volume_high_flow_flag_consistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_volume_metrics
                WHERE is_high_short_flow
                  AND (
                      coalesce(short_volume_ratio_percentile, 0) < 0.90
                      OR coalesce(total_volume, 0) <= 0
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_volume_metrics",),
        ),
    )
