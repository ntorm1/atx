from __future__ import annotations

from ._check_common import DEFAULT_EXPORT_OBJECTS, _export_scan_internal_cusip_sql
from ._types import SqlQualityCheck


def feature_catalog_check_specs(
    *,
    daily_macro_stale_days: int,
    monthly_macro_stale_days: int,
    valuation_stale_gap_days: int,
) -> tuple[SqlQualityCheck, ...]:
    return (
        SqlQualityCheck(
            dataset_id="fred_macro",
            table_name="macro_observations",
            check_name="duplicate_macro_observations",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, series_id, observation_date, as_of_date, count(*) AS row_count
                    FROM macro_observations
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("macro_observations",),
        ),
        SqlQualityCheck(
            dataset_id="fred_macro",
            table_name="macro_series",
            check_name="macro_observations_without_series_metadata",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT source, series_id
                    FROM macro_observations
                ) o
                LEFT JOIN macro_series s
                  ON s.source = o.source
                 AND s.series_id = o.series_id
                WHERE s.series_id IS NULL
            """,
            threshold=0.0,
            required_tables=("macro_observations", "macro_series"),
        ),
        SqlQualityCheck(
            dataset_id="fred_macro",
            table_name="macro_observations",
            check_name="stale_daily_macro_observations",
            sql=f"""
                SELECT coalesce(max(date_diff('day', latest_date, current_date)), 0)::DOUBLE
                FROM (
                    SELECT series_id, max(observation_date) AS latest_date
                    FROM macro_observations
                    WHERE series_id IN ('DGS10', 'DGS2', 'VIXCLS')
                    GROUP BY series_id
                )
            """,
            threshold=float(daily_macro_stale_days),
            comparator="le",
            required_tables=("macro_observations",),
        ),
        SqlQualityCheck(
            dataset_id="fred_macro",
            table_name="macro_observations",
            check_name="stale_monthly_macro_observations",
            sql=f"""
                SELECT coalesce(max(date_diff('day', latest_date, current_date)), 0)::DOUBLE
                FROM (
                    SELECT series_id, max(observation_date) AS latest_date
                    FROM macro_observations
                    WHERE series_id IN ('FEDFUNDS', 'UNRATE', 'CPIAUCSL')
                    GROUP BY series_id
                )
            """,
            threshold=float(monthly_macro_stale_days),
            comparator="le",
            required_tables=("macro_observations",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="duplicate_feature_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT feature_set, feature_name, security_id, as_of_date, count(*) AS row_count
                    FROM feature_values
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_values",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="orphan_feature_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_values f
                LEFT JOIN securities s ON s.security_id = f.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_values", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="missing_feature_available_at",
            sql="SELECT count(*)::DOUBLE FROM feature_values WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("feature_values",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="missing_feature_input_hash",
            sql="SELECT count(*)::DOUBLE FROM feature_values WHERE input_hash IS NULL OR input_hash = ''",
            threshold=0.0,
            required_tables=("feature_values",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_definitions",
            check_name="duplicate_feature_definitions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT feature_set, feature_name, count(*) AS row_count
                    FROM feature_definitions
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_definitions",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="feature_values_without_definition",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT feature_set, feature_name
                    FROM feature_values
                ) v
                LEFT JOIN feature_definitions d
                  ON d.feature_set = v.feature_set
                 AND d.feature_name = v.feature_name
                WHERE d.feature_name IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_values", "feature_definitions"),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_build_manifests",
            check_name="duplicate_feature_build_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT feature_set, run_id, count(*) AS row_count
                    FROM feature_build_manifests
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_build_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_build_manifests",
            check_name="bad_feature_build_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_build_manifests
                WHERE feature_set IS NULL
                   OR feature_set = ''
                   OR symbols_json IS NULL
                   OR feature_names_json IS NULL
                   OR input_tables_json IS NULL
                   OR input_row_count < 0
                   OR output_row_count < 0
                   OR feature_count <= 0
                   OR (input_min_as_of_date IS NOT NULL AND input_max_as_of_date < input_min_as_of_date)
                   OR (output_min_as_of_date IS NOT NULL AND output_max_as_of_date < output_min_as_of_date)
                   OR (min_available_at IS NOT NULL AND max_available_at < min_available_at)
            """,
            threshold=0.0,
            required_tables=("feature_build_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="feature_values_without_build_manifest",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT feature_set, run_id
                    FROM feature_values
                    WHERE run_id IS NOT NULL
                      AND run_id <> ''
                ) v
                LEFT JOIN feature_build_manifests m
                  ON m.feature_set = v.feature_set
                 AND m.run_id = v.run_id
                WHERE m.manifest_id IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_values", "feature_build_manifests"),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_set_catalog",
            check_name="duplicate_feature_set_catalog_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT feature_set, count(*) AS row_count
                    FROM feature_set_catalog
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_set_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_set_catalog",
            check_name="bad_feature_set_catalog_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_set_catalog
                WHERE feature_set IS NULL
                   OR feature_set = ''
                   OR feature_family IS NULL
                   OR feature_family = ''
                   OR feature_count <= 0
                   OR dependency_count < 0
                   OR source_table_count < 0
                   OR derived_feature_dependency_count < 0
                   OR input_tables_json IS NULL
                   OR input_tables_json IN ('', '[]')
                   OR feature_names_json IS NULL
                   OR feature_names_json IN ('', '[]')
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("feature_set_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_dependency_edges",
            check_name="duplicate_feature_dependency_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT dependency_id, count(*) AS row_count
                    FROM feature_dependency_edges
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_dependency_edges",),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_dependency_edges",
            check_name="bad_feature_dependency_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_dependency_edges
                WHERE dependency_id IS NULL
                   OR dependency_id = ''
                   OR feature_set IS NULL
                   OR feature_set = ''
                   OR feature_name IS NULL
                   OR feature_name = ''
                   OR dependency_type NOT IN ('source_table', 'derived_feature')
                   OR dependency_name IS NULL
                   OR dependency_name = ''
                   OR dependency_depth < 1
                   OR source IS NULL
                   OR source = ''
                   OR (dependency_type = 'source_table' AND dependency_feature_name IS NOT NULL)
                   OR (dependency_type = 'derived_feature' AND (
                       dependency_feature_set IS NULL
                       OR dependency_feature_name IS NULL
                   ))
            """,
            threshold=0.0,
            required_tables=("feature_dependency_edges",),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_dependency_edges",
            check_name="feature_definitions_without_dependency_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_definitions d
                LEFT JOIN feature_dependency_edges e
                  ON e.feature_set = d.feature_set
                 AND e.feature_name = d.feature_name
                WHERE e.dependency_id IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_definitions", "feature_dependency_edges"),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_dependency_edges",
            check_name="derived_feature_edges_without_definition",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_dependency_edges e
                LEFT JOIN feature_definitions d
                  ON d.feature_set = e.dependency_feature_set
                 AND d.feature_name = e.dependency_feature_name
                WHERE e.dependency_type = 'derived_feature'
                  AND d.feature_name IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_dependency_edges", "feature_definitions"),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_expression_catalog",
            check_name="duplicate_alpha_expression_catalog",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT alpha_id, count(*) AS row_count
                    FROM alpha_expression_catalog
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("alpha_expression_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_expression_catalog",
            check_name="bad_alpha_expression_catalog_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_expression_catalog
                WHERE alpha_id IS NULL
                   OR alpha_id = ''
                   OR alpha_name IS NULL
                   OR alpha_name = ''
                   OR expression_sql IS NULL
                   OR expression_sql = ''
                   OR feature_set IS NULL
                   OR feature_set = ''
                   OR input_features_json IS NULL
                   OR input_features_json = ''
                   OR rebalance_frequency IS NULL
                   OR rebalance_frequency = ''
                   OR horizon_days < 1
                   OR direction NOT IN (-1, 1)
                   OR NOT is_point_in_time_safe
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("alpha_expression_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_signal_values",
            check_name="duplicate_alpha_signal_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT alpha_id, security_id, as_of_date, count(*) AS row_count
                    FROM alpha_signal_values
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("alpha_signal_values",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_signal_values",
            check_name="bad_alpha_signal_value_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_signal_values
                WHERE alpha_signal_id IS NULL
                   OR alpha_signal_id = ''
                   OR alpha_id IS NULL
                   OR alpha_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR as_of_date IS NULL
                   OR signal_value IS NULL
                   OR rank_value IS NULL
                   OR rank_value < 0
                   OR rank_value > 1
                   OR weight IS NULL
                   OR cross_section_count < 2
                   OR available_at IS NULL
                   OR input_hash IS NULL
                   OR input_hash = ''
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("alpha_signal_values",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_signal_values",
            check_name="alpha_signal_values_without_catalog",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_signal_values s
                LEFT JOIN alpha_expression_catalog c
                  ON c.alpha_id = s.alpha_id
                WHERE c.alpha_id IS NULL
            """,
            threshold=0.0,
            required_tables=("alpha_signal_values", "alpha_expression_catalog"),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_signal_values",
            check_name="alpha_signal_values_without_security",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_signal_values a
                LEFT JOIN securities s
                  ON s.security_id = a.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("alpha_signal_values", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_backtest_manifests",
            check_name="duplicate_alpha_backtest_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT backtest_id, count(*) AS row_count
                    FROM alpha_backtest_manifests
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("alpha_backtest_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_backtest_manifests",
            check_name="bad_alpha_backtest_manifest_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_backtest_manifests
                WHERE backtest_id IS NULL
                   OR backtest_id = ''
                   OR alpha_id IS NULL
                   OR alpha_id = ''
                   OR feature_set IS NULL
                   OR feature_set = ''
                   OR horizon_days < 1
                   OR signal_count < 1
                   OR security_count < 1
                   OR evaluation_days < 1
                   OR evaluated_signal_count < 1
                   OR (start_date IS NOT NULL AND end_date < start_date)
                   OR (min_available_at IS NOT NULL AND max_available_at < min_available_at)
                   OR hit_rate < 0
                   OR hit_rate > 1
                   OR params_json IS NULL
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("alpha_backtest_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_backtest_manifests",
            check_name="alpha_backtest_manifests_without_catalog",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_backtest_manifests b
                LEFT JOIN alpha_expression_catalog c
                  ON c.alpha_id = b.alpha_id
                WHERE c.alpha_id IS NULL
            """,
            threshold=0.0,
            required_tables=("alpha_backtest_manifests", "alpha_expression_catalog"),
        ),
        SqlQualityCheck(
            dataset_id="universe_memberships",
            table_name="universe_memberships",
            check_name="duplicate_universe_memberships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT universe_id, security_id, effective_date, as_of_date, count(*) AS row_count
                    FROM universe_memberships
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("universe_memberships",),
        ),
        SqlQualityCheck(
            dataset_id="universe_memberships",
            table_name="universe_memberships",
            check_name="orphan_universe_memberships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM universe_memberships u
                LEFT JOIN securities s ON s.security_id = u.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("universe_memberships", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="universe_memberships",
            table_name="universe_memberships",
            check_name="missing_universe_available_at",
            sql="SELECT count(*)::DOUBLE FROM universe_memberships WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("universe_memberships",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_lake_exports",
            table_name="lake_export_files",
            check_name="duplicate_lake_export_files",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT export_run_id, object_name, count(*) AS row_count
                    FROM lake_export_files
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("lake_export_files",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_lake_exports",
            table_name="lake_export_runs",
            check_name="bad_lake_export_runs",
            sql="""
                SELECT count(*)::DOUBLE
                FROM lake_export_runs
                WHERE status NOT IN ('running', 'succeeded', 'failed')
                   OR object_count < 0
                   OR coalesce(total_rows, 0) < 0
                   OR coalesce(total_byte_count, 0) < 0
                   OR (status = 'succeeded' AND finished_at IS NULL)
                   OR (status = 'failed' AND coalesce(error_message, '') = '')
            """,
            threshold=0.0,
            required_tables=("lake_export_runs",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_lake_exports",
            table_name="lake_export_files",
            check_name="bad_lake_export_files",
            sql="""
                SELECT count(*)::DOUBLE
                FROM lake_export_files
                WHERE object_name IS NULL
                   OR object_name = ''
                   OR output_path IS NULL
                   OR output_path = ''
                   OR manifest_path IS NULL
                   OR manifest_path = ''
                   OR rows < 0
                   OR byte_count <= 0
                   OR sha256 IS NULL
                   OR length(sha256) <> 64
                   OR schema_sha256 IS NULL
                   OR length(schema_sha256) <> 64
            """,
            threshold=0.0,
            required_tables=("lake_export_files",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_lake_exports",
            table_name="lake_export_runs",
            check_name="incomplete_lake_export_runs",
            sql="""
                WITH file_totals AS (
                    SELECT
                        export_run_id,
                        count(*) AS file_count,
                        sum(rows) AS total_rows,
                        sum(byte_count) AS total_byte_count
                    FROM lake_export_files
                    GROUP BY 1
                )
                SELECT count(*)::DOUBLE
                FROM lake_export_runs r
                LEFT JOIN file_totals f ON f.export_run_id = r.export_run_id
                WHERE r.status = 'succeeded'
                  AND (
                      coalesce(f.file_count, 0) <> r.object_count
                      OR coalesce(f.total_rows, 0) <> coalesce(r.total_rows, 0)
                      OR coalesce(f.total_byte_count, 0) <> coalesce(r.total_byte_count, 0)
                  )
            """,
            threshold=0.0,
            required_tables=("lake_export_runs", "lake_export_files"),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="dataset_watermarks",
            check_name="bad_dataset_watermarks",
            sql="""
                SELECT count(*)::DOUBLE
                FROM dataset_watermarks
                WHERE dataset_id IS NULL
                   OR dataset_id = ''
                   OR watermark_name IS NULL
                   OR watermark_name = ''
                   OR watermark_value IS NULL
                   OR watermark_value = ''
            """,
            threshold=0.0,
            required_tables=("dataset_watermarks",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="dataset_watermarks",
            check_name="orphan_dataset_watermarks",
            sql="""
                SELECT count(*)::DOUBLE
                FROM dataset_watermarks w
                LEFT JOIN dataset_catalog d ON d.dataset_id = w.dataset_id
                WHERE d.dataset_id IS NULL
            """,
            threshold=0.0,
            required_tables=("dataset_watermarks", "dataset_catalog"),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="dataset_watermarks",
            check_name="missing_core_dataset_watermarks",
            sql="""
                WITH expected AS (
                    SELECT 'tbltickerhistory_daily' AS dataset_id, 'max_trade_date' AS watermark_name, count(*) AS row_count FROM tbltickerhistory_daily
                    UNION ALL
                    SELECT 'corporate_actions', 'max_ex_date', count(*) FROM corporate_actions
                    UNION ALL
                    SELECT 'corp_action_type_dim', 'max_updated_at', count(*) FROM corp_action_type_dim
                    UNION ALL
                    SELECT 'adjustment_factor_history', 'max_ex_date', count(*) FROM adjustment_factor_history
                    UNION ALL
                    SELECT 'adjustment_factor_history', 'max_available_at', count(*) FROM adjustment_factor_history
                    UNION ALL
                    SELECT 'daily_adjustment_factors', 'max_trade_date', count(*) FROM daily_adjustment_factors
                    UNION ALL
                    SELECT 'daily_adjustment_factors', 'max_as_of_date', count(*) FROM daily_adjustment_factors
                    UNION ALL
                    SELECT 'daily_adjustment_factors', 'max_available_at', count(*) FROM daily_adjustment_factors
                    UNION ALL
                    SELECT 'corporate_action_factor_reconciliation', 'max_ex_date', count(*) FROM corporate_action_factor_reconciliation
                    UNION ALL
                    SELECT 'corporate_action_factor_reconciliation', 'max_available_at', count(*) FROM corporate_action_factor_reconciliation
                    UNION ALL
                    SELECT 'shares_outstanding_history', 'max_effective_date', count(*) FROM shares_outstanding_history
                    UNION ALL
                    SELECT 'shares_outstanding_history', 'max_available_at', count(*) FROM shares_outstanding_history
                    UNION ALL
                    SELECT 'finra_short_interest', 'max_settlement_date', count(*) FROM finra_short_interest
                    UNION ALL
                    SELECT 'finra_short_interest_backfills', 'last_finished_at', count(*) FROM finra_short_interest_backfill_manifests
                    UNION ALL
                    SELECT 'finra_short_interest_features', 'max_as_of_date:finra_short_interest_v1', count(*) FROM feature_values WHERE feature_set = 'finra_short_interest_v1'
                    UNION ALL
                    SELECT 'sec_13f', 'source_period', count(*) FROM thirteenf_submissions
                    UNION ALL
                    SELECT 'sec_13f_ownership_features', 'max_report_period', count(*) FROM thirteenf_security_ownership
                    UNION ALL
                    SELECT 'sec_13f_ownership_features', 'max_available_at', count(*) FROM thirteenf_security_ownership
                    UNION ALL
                    SELECT 'sec_13f_ownership_features', 'max_as_of_date:sec_13f_ownership_v1', count(*) FROM feature_values WHERE feature_set = 'sec_13f_ownership_v1'
                    UNION ALL
                    SELECT 'sec_company_facts', 'max_available_at', count(*) FROM fundamental_points
                    UNION ALL
                    SELECT 'xbrl_concept_catalog', 'max_available_at', count(*) FROM xbrl_concept_catalog
                    UNION ALL
                    SELECT 'xbrl_concept_catalog', 'max_updated_at', count(*) FROM xbrl_concept_catalog
                    UNION ALL
                    SELECT 'xbrl_taxonomy', 'max_release_year', count(*) FROM xbrl_taxonomy_packages
                    UNION ALL
                    SELECT 'xbrl_taxonomy', 'max_source_loaded_at', count(*) FROM xbrl_taxonomy_relationships
                    UNION ALL
                    SELECT 'xbrl_dimensions', 'max_source_loaded_at', count(*) FROM xbrl_dimension_edges
                    UNION ALL
                    SELECT 'xbrl_fact_frames', 'max_available_at', count(*) FROM xbrl_fact_frames
                    UNION ALL
                    SELECT 'xbrl_fact_frames', 'max_updated_at', count(*) FROM xbrl_fact_frames
                    UNION ALL
                    SELECT 'xbrl_filing_contexts', 'max_acceptance_datetime', count(*) FROM xbrl_filing_contexts
                    UNION ALL
                    SELECT 'xbrl_filing_contexts', 'max_source_loaded_at', count(*) FROM xbrl_filing_contexts
                    UNION ALL
                    SELECT 'xbrl_filing_dimensions', 'max_acceptance_datetime', count(*) FROM xbrl_filing_dimensions
                    UNION ALL
                    SELECT 'xbrl_filing_facts', 'max_acceptance_datetime', count(*) FROM xbrl_filing_facts
                    UNION ALL
                    SELECT 'xbrl_filing_facts', 'max_source_loaded_at', count(*) FROM xbrl_filing_facts
                    UNION ALL
                    SELECT 'fundamental_fact_revisions', 'max_available_at', count(*) FROM fundamental_fact_revisions
                    UNION ALL
                    SELECT 'fundamental_fact_revisions', 'max_updated_at', count(*) FROM fundamental_fact_revisions
                    UNION ALL
                    SELECT 'fundamental_statement_map', 'max_updated_at', count(*) FROM fundamental_statement_map
                    UNION ALL
                    SELECT 'fundamental_statement_points', 'max_available_at', count(*) FROM fundamental_statement_points
                    UNION ALL
                    SELECT 'fundamental_statement_points', 'max_updated_at', count(*) FROM fundamental_statement_points
                    UNION ALL
                    SELECT 'fundamental_ttm_points', 'max_available_at', count(*) FROM fundamental_ttm_points
                    UNION ALL
                    SELECT 'fundamental_ttm_points', 'max_updated_at', count(*) FROM fundamental_ttm_points
                    UNION ALL
                    SELECT 'fundamental_periods', 'max_available_at', count(*) FROM fundamental_periods
                    UNION ALL
                    SELECT 'fundamental_periods', 'max_updated_at', count(*) FROM fundamental_periods
                    UNION ALL
                    SELECT 'sec_submissions', 'max_filing_date', count(*) FROM sec_submissions
                    UNION ALL
                    SELECT 'nasdaq_symbol_directory', 'max_as_of_date', count(*) FROM nasdaq_symbol_directory
                    UNION ALL
                    SELECT 'nasdaq_listing_events', 'max_effective_date', count(*) FROM nasdaq_listing_events
                    UNION ALL
                    SELECT 'nasdaq_listing_events', 'max_as_of_date', count(*) FROM nasdaq_listing_events
                    UNION ALL
                    SELECT 'nasdaq_listing_events', 'max_source_file_created_at', count(*) FROM nasdaq_listing_events
                    UNION ALL
                    SELECT 'listing_status_intervals', 'max_valid_from', count(*) FROM listing_status_intervals
                    UNION ALL
                    SELECT 'listing_status_intervals', 'max_available_at', count(*) FROM listing_status_intervals
                    UNION ALL
                    SELECT 'listing_status_intervals', 'max_last_evidence_as_of_date', count(*) FROM listing_status_intervals
                    UNION ALL
                    SELECT 'delist_code_dim', 'max_updated_at', count(*) FROM delist_code_dim
                    UNION ALL
                    SELECT 'delisting_events', 'max_delist_date', count(*) FROM delisting_events
                    UNION ALL
                    SELECT 'delisting_events', 'max_as_of_date', count(*) FROM delisting_events
                    UNION ALL
                    SELECT 'delisting_events', 'max_available_at', count(*) FROM delisting_events
                    UNION ALL
                    SELECT 'delisting_return_observations', 'max_delist_date', count(*) FROM delisting_return_observations
                    UNION ALL
                    SELECT 'delisting_return_observations', 'max_as_of_date', count(*) FROM delisting_return_observations
                    UNION ALL
                    SELECT 'delisting_return_observations', 'max_available_at', count(*) FROM delisting_return_observations
                    UNION ALL
                    SELECT 'fred_macro', 'max_observation_date', count(*) FROM macro_observations
                    UNION ALL
                    SELECT 'trading_calendar', 'max_trade_date', count(*) FROM trading_calendar
                    UNION ALL
                    SELECT 'universe_memberships', 'max_as_of_date', count(*) FROM universe_memberships
                    UNION ALL
                    SELECT 'equity_daily_features', 'max_as_of_date:equity_daily_v1', count(*) FROM feature_values WHERE feature_set = 'equity_daily_v1'
                    UNION ALL
                    SELECT 'sec_fundamental_features', 'max_as_of_date:sec_fundamentals_v1', count(*) FROM feature_values WHERE feature_set = 'sec_fundamentals_v1'
                    UNION ALL
                    SELECT 'feature_lineage', 'max_updated_at', count(*) FROM feature_dependency_edges
                    UNION ALL
                    SELECT 'alpha_research', 'max_as_of_date', count(*) FROM alpha_signal_values
                    UNION ALL
                    SELECT 'alpha_research', 'max_available_at', count(*) FROM alpha_signal_values
                    UNION ALL
                    SELECT 'alpha_research', 'max_backtest_end_date', count(*) FROM alpha_backtest_manifests
                    UNION ALL
                    SELECT 'warehouse_lake_exports', 'last_succeeded_export', count(*) FROM lake_export_runs WHERE status = 'succeeded'
                )
                SELECT count(*)::DOUBLE
                FROM expected e
                LEFT JOIN dataset_watermarks w
                  ON w.dataset_id = e.dataset_id
                 AND w.watermark_name = e.watermark_name
                WHERE e.row_count > 0
                  AND w.watermark_value IS NULL
            """,
            threshold=0.0,
            required_tables=(
                "dataset_watermarks",
                "tbltickerhistory_daily",
                "corporate_actions",
                "corp_action_type_dim",
                "adjustment_factor_history",
                "daily_adjustment_factors",
                "corporate_action_factor_reconciliation",
                "shares_outstanding_history",
                "finra_short_interest",
                "finra_short_interest_backfill_manifests",
                "thirteenf_submissions",
                "thirteenf_security_ownership",
                "fundamental_points",
                "xbrl_concept_catalog",
                "xbrl_taxonomy_packages",
                "xbrl_taxonomy_relationships",
                "xbrl_dimension_edges",
                "xbrl_fact_frames",
                "xbrl_filing_contexts",
                "xbrl_filing_dimensions",
                "xbrl_filing_facts",
                "fundamental_fact_revisions",
                "fundamental_statement_map",
                "fundamental_statement_points",
                "fundamental_ttm_points",
                "fundamental_periods",
                "sec_submissions",
                "nasdaq_symbol_directory",
                "nasdaq_listing_events",
                "listing_status_intervals",
                "delist_code_dim",
                "delisting_events",
                "delisting_return_observations",
                "macro_observations",
                "trading_calendar",
                "universe_memberships",
                "feature_values",
                "feature_dependency_edges",
                "alpha_signal_values",
                "alpha_backtest_manifests",
                "lake_export_runs",
            ),
        ),
        SqlQualityCheck(
            dataset_id="provider_parity_matrix",
            table_name="provider_parity_matrix",
            check_name="duplicate_provider_parity_domains",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT provider, provider_domain, count(*) AS row_count
                    FROM provider_parity_matrix
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("provider_parity_matrix",),
        ),
        SqlQualityCheck(
            dataset_id="provider_parity_matrix",
            table_name="provider_parity_matrix",
            check_name="bad_provider_parity_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM provider_parity_matrix
                WHERE provider IS NULL
                   OR provider = ''
                   OR provider_domain IS NULL
                   OR provider_domain = ''
                   OR warehouse_domain IS NULL
                   OR warehouse_domain = ''
                   OR institutional_grain IS NULL
                   OR institutional_grain = ''
                   OR institutional_keys_json IS NULL
                   OR institutional_keys_json IN ('', '[]')
                   OR pit_fields_json IS NULL
                   OR pit_fields_json IN ('', '[]')
                   OR factors_or_fields_json IS NULL
                   OR factors_or_fields_json IN ('', '[]')
                   OR open_substitute IS NULL
                   OR open_substitute = ''
                   OR warehouse_tables_json IS NULL
                   OR warehouse_tables_json IN ('', '[]')
                   OR source_urls_json IS NULL
                   OR source_urls_json IN ('', '[]')
                   OR parity_status NOT IN ('implemented', 'partial', 'planned', 'research_only')
            """,
            threshold=0.0,
            required_tables=("provider_parity_matrix",),
        ),
        SqlQualityCheck(
            dataset_id="provider_parity_matrix",
            table_name="provider_parity_matrix",
            check_name="provider_parity_rows_without_open_tables",
            sql="""
                SELECT count(*)::DOUBLE
                FROM provider_parity_matrix
                WHERE warehouse_tables_json NOT LIKE '%securities%'
                  AND warehouse_tables_json NOT LIKE '%equity_daily_bars%'
                  AND warehouse_tables_json NOT LIKE '%feature_values%'
                  AND warehouse_tables_json NOT LIKE '%fundamental_points%'
                  AND warehouse_tables_json NOT LIKE '%thirteenf_holdings%'
                  AND warehouse_tables_json NOT LIKE '%thirteenf_security_ownership%'
                  AND warehouse_tables_json NOT LIKE '%finra_short_interest%'
                  AND warehouse_tables_json NOT LIKE '%macro_observations%'
                  AND warehouse_tables_json NOT LIKE '%nasdaq_symbol_directory%'
                  AND warehouse_tables_json NOT LIKE '%nasdaq_listing_events%'
                  AND warehouse_tables_json NOT LIKE '%listing_status_intervals%'
                  AND warehouse_tables_json NOT LIKE '%dataset_catalog%'
            """,
            threshold=0.0,
            required_tables=("provider_parity_matrix",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_jobs",
            table_name="etl_job_definitions",
            check_name="bad_etl_job_retry_policy",
            sql="""
                SELECT count(*)::DOUBLE
                FROM etl_job_definitions
                WHERE max_retries < 0
                   OR retry_delay_seconds < 0
            """,
            threshold=0.0,
            required_tables=("etl_job_definitions",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_jobs",
            table_name="etl_job_runs",
            check_name="bad_etl_job_run_retry_metadata",
            sql="""
                SELECT count(*)::DOUBLE
                FROM etl_job_runs
                WHERE attempt_count < 0
                   OR max_retries < 0
                   OR retry_delay_seconds < 0
                   OR attempt_count > max_retries + 1
            """,
            threshold=0.0,
            required_tables=("etl_job_runs",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="field_catalog",
            check_name="missing_field_catalog_entries",
            sql="""
                SELECT count(*)::DOUBLE
                FROM duckdb_columns() c
                WHERE c.schema_name = 'main'
                  AND coalesce(c.internal, false) = false
                  AND c.table_name NOT LIKE 'duckdb_%'
                  AND c.table_name NOT LIKE 'sqlite_%'
                  AND c.table_name NOT LIKE 'pragma_%'
                  AND NOT EXISTS (
                      SELECT 1
                      FROM field_catalog f
                      WHERE f.table_name = c.table_name
                        AND f.field_name = c.column_name
                  )
            """,
            threshold=0.0,
            required_tables=("field_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="table_catalog",
            check_name="missing_table_catalog_entries",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT table_name AS object_name
                    FROM duckdb_tables()
                    WHERE schema_name = 'main'
                      AND coalesce(internal, false) = false
                    UNION
                    SELECT view_name AS object_name
                    FROM duckdb_views()
                    WHERE schema_name = 'main'
                      AND coalesce(internal, false) = false
                      AND view_name NOT LIKE 'duckdb_%'
                      AND view_name NOT LIKE 'sqlite_%'
                      AND view_name NOT LIKE 'pragma_%'
                ) o
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM table_catalog t
                    WHERE t.table_name = o.object_name
                )
            """,
            threshold=0.0,
            required_tables=("table_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="entity_classification",
            table_name="entity_classification",
            check_name="orphan_entity_classification_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM entity_classification ec
                LEFT JOIN securities s ON s.security_id = ec.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("entity_classification", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="entity_classification",
            table_name="entity_classification",
            check_name="entity_classification_invalid_node_references",
            sql="""
                SELECT count(*)::DOUBLE
                FROM entity_classification ec
                LEFT JOIN taxonomy_node tn
                  ON tn.node_id = ec.node_id
                 AND tn.node_code = ec.node_code
                WHERE tn.node_id IS NULL
            """,
            threshold=0.0,
            required_tables=("entity_classification", "taxonomy_node"),
        ),
        SqlQualityCheck(
            dataset_id="entity_classification",
            table_name="entity_classification",
            check_name="entity_classification_multiple_open_intervals",
            # At most ONE open (valid_to IS NULL) interval per
            # (security_id, taxonomy_id) — covers primary SIC AND derived
            # FF12/NAICS rows, so cross-boundary reclassification that fails to
            # close a stale derived interval is caught here, not just for SIC.
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, taxonomy_id, count(*) AS open_count
                    FROM entity_classification
                    WHERE valid_to IS NULL
                    GROUP BY security_id, taxonomy_id
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("entity_classification",),
        ),
    )
