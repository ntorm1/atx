from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

import duckdb

from .connection import DuckDBStore


WATERMARK_QUERIES: tuple[str, ...] = (
    """
    SELECT 'tbltickerhistory_daily', 'min_trade_date', min(trading_date)::VARCHAR
    FROM tbltickerhistory_daily
    HAVING count(*) > 0
    """,
    """
    SELECT 'tbltickerhistory_daily', 'max_trade_date', max(trading_date)::VARCHAR
    FROM tbltickerhistory_daily
    HAVING count(*) > 0
    """,
    """
    SELECT 'corporate_actions', 'max_ex_date', max(ex_date)::VARCHAR
    FROM corporate_actions
    HAVING count(*) > 0
    """,
    """
    SELECT 'corp_action_type_dim', 'max_updated_at', max(updated_at)::VARCHAR
    FROM corp_action_type_dim
    HAVING count(*) > 0
    """,
    """
    SELECT 'adjustment_factor_history', 'max_ex_date', max(ex_date)::VARCHAR
    FROM adjustment_factor_history
    HAVING count(*) > 0
    """,
    """
    SELECT 'adjustment_factor_history', 'max_available_at', max(available_at)::VARCHAR
    FROM adjustment_factor_history
    HAVING count(*) > 0
    """,
    """
    SELECT 'daily_adjustment_factors', 'max_trade_date', max(trade_date)::VARCHAR
    FROM daily_adjustment_factors
    HAVING count(*) > 0
    """,
    """
    SELECT 'daily_adjustment_factors', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM daily_adjustment_factors
    HAVING count(*) > 0
    """,
    """
    SELECT 'daily_adjustment_factors', 'max_available_at', max(available_at)::VARCHAR
    FROM daily_adjustment_factors
    HAVING count(*) > 0
    """,
    """
    SELECT 'shares_outstanding_history', 'max_effective_date', max(effective_date)::VARCHAR
    FROM shares_outstanding_history
    HAVING count(*) > 0
    """,
    """
    SELECT 'shares_outstanding_history', 'max_available_at', max(available_at)::VARCHAR
    FROM shares_outstanding_history
    HAVING count(*) > 0
    """,
    """
    SELECT 'finra_short_interest', 'max_settlement_date', max(settlement_date)::VARCHAR
    FROM finra_short_interest
    HAVING count(*) > 0
    """,
    """
    SELECT 'offexchange_volume', 'max_summary_start_date', max(summary_start_date)::VARCHAR
    FROM offexchange_volume
    HAVING count(*) > 0
    """,
    """
    SELECT 'offexchange_volume', 'max_available_at', max(available_at)::VARCHAR
    FROM offexchange_volume
    HAVING count(*) > 0
    """,
    """
    SELECT 'offexchange_security_period', 'max_summary_start_date', max(summary_start_date)::VARCHAR
    FROM offexchange_security_period
    HAVING count(*) > 0
    """,
    """
    SELECT 'finra_short_interest_backfills', 'last_finished_at', max(finished_at)::VARCHAR
    FROM finra_short_interest_backfill_manifests
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_13f', 'max_report_period', max(period_of_report)::VARCHAR
    FROM thirteenf_submissions
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_13f', 'source_period', max(source_period)::VARCHAR
    FROM thirteenf_submissions
    HAVING count(*) > 0
    """,
    """
    SELECT 'filer_13f_cik_alias', 'max_valid_from', max(valid_from)::VARCHAR
    FROM filer_13f_cik_alias
    HAVING count(*) > 0
    """,
    """
    SELECT 'filer_13f_cik_alias', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM filer_13f_cik_alias
    HAVING count(*) > 0
    """,
    """
    SELECT 'filer_13f_cik_alias', 'max_available_at', max(available_at)::VARCHAR
    FROM filer_13f_cik_alias
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_13f_ownership_features', 'max_report_period', max(report_period)::VARCHAR
    FROM thirteenf_security_ownership
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_13f_ownership_features', 'max_available_at', max(available_at)::VARCHAR
    FROM thirteenf_security_ownership
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_13f_ownership_features', 'max_as_of_date:sec_13f_ownership_v1', max(as_of_date)::VARCHAR
    FROM feature_values
    WHERE feature_set = 'sec_13f_ownership_v1'
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_insider_ownership', 'max_period_of_report', max(period_of_report)::VARCHAR
    FROM filing_form4
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_insider_ownership', 'max_available_at', max(available_at)::VARCHAR
    FROM filing_form4
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_insider_ownership', 'max_transaction_date', max(transaction_date)::VARCHAR
    FROM insider_transaction
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_insider_ownership', 'max_10b5_1_adoption_date', max(adoption_date)::VARCHAR
    FROM tradingplan_10b5_1
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_blockholder_ownership', 'max_event_date', max(event_date)::VARCHAR
    FROM blockholder_filing
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_blockholder_ownership', 'max_available_at', max(available_at)::VARCHAR
    FROM blockholder_filing
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_company_facts', 'max_period_end', max(period_end)::VARCHAR
    FROM fundamental_points
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_company_facts', 'max_available_at', max(available_at)::VARCHAR
    FROM fundamental_points
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_concept_catalog', 'max_available_at', max(last_available_at)::VARCHAR
    FROM xbrl_concept_catalog
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_concept_catalog', 'max_updated_at', max(updated_at)::VARCHAR
    FROM xbrl_concept_catalog
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_taxonomy', 'max_release_year', max(release_year)::VARCHAR
    FROM xbrl_taxonomy_packages
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_taxonomy', 'max_source_loaded_at', max(source_loaded_at)::VARCHAR
    FROM xbrl_taxonomy_relationships
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_dimensions', 'max_source_loaded_at', max(source_loaded_at)::VARCHAR
    FROM xbrl_dimension_edges
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_fact_frames', 'max_available_at', max(last_available_at)::VARCHAR
    FROM xbrl_fact_frames
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_fact_frames', 'max_updated_at', max(updated_at)::VARCHAR
    FROM xbrl_fact_frames
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_filing_contexts', 'max_acceptance_datetime', max(acceptance_datetime)::VARCHAR
    FROM xbrl_filing_contexts
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_filing_contexts', 'max_source_loaded_at', max(source_loaded_at)::VARCHAR
    FROM xbrl_filing_contexts
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_filing_dimensions', 'max_acceptance_datetime', max(acceptance_datetime)::VARCHAR
    FROM xbrl_filing_dimensions
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_filing_facts', 'max_acceptance_datetime', max(acceptance_datetime)::VARCHAR
    FROM xbrl_filing_facts
    HAVING count(*) > 0
    """,
    """
    SELECT 'xbrl_filing_facts', 'max_source_loaded_at', max(source_loaded_at)::VARCHAR
    FROM xbrl_filing_facts
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_fact_revisions', 'max_available_at', max(available_at)::VARCHAR
    FROM fundamental_fact_revisions
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_fact_revisions', 'max_updated_at', max(updated_at)::VARCHAR
    FROM fundamental_fact_revisions
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_statement_map', 'max_updated_at', max(updated_at)::VARCHAR
    FROM fundamental_statement_map
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_statement_points', 'max_available_at', max(available_at)::VARCHAR
    FROM fundamental_statement_points
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_statement_points', 'max_updated_at', max(updated_at)::VARCHAR
    FROM fundamental_statement_points
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_ttm_points', 'max_available_at', max(available_at)::VARCHAR
    FROM fundamental_ttm_points
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_ttm_points', 'max_updated_at', max(updated_at)::VARCHAR
    FROM fundamental_ttm_points
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_periods', 'max_available_at', max(available_at)::VARCHAR
    FROM fundamental_periods
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_periods', 'max_updated_at', max(updated_at)::VARCHAR
    FROM fundamental_periods
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_ratios', 'max_available_at', max(available_at)::VARCHAR
    FROM fundamental_ratios
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_ratios', 'max_period_end', max(period_end)::VARCHAR
    FROM fundamental_ratios
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_xbrl_metric', 'max_available_at', max(available_at)::VARCHAR
    FROM fundamental_xbrl_metric
    HAVING count(*) > 0
    """,
    """
    SELECT 'fundamental_xbrl_metric', 'max_period_end', max(period_end)::VARCHAR
    FROM fundamental_xbrl_metric
    HAVING count(*) > 0
    """,
    """
    SELECT 'short_interest_metrics', 'max_settlement_date', max(settlement_date)::VARCHAR
    FROM short_interest_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'short_interest_metrics', 'max_available_at', max(available_at)::VARCHAR
    FROM short_interest_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'macro_metrics', 'max_observation_date', max(observation_date)::VARCHAR
    FROM macro_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'macro_metrics', 'max_available_at', max(available_at)::VARCHAR
    FROM macro_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'equity_price_metrics', 'max_trade_date', max(trade_date)::VARCHAR
    FROM equity_price_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'equity_price_metrics', 'max_available_at', max(available_at)::VARCHAR
    FROM equity_price_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'thirteenf_position_metrics', 'max_report_period', max(report_period)::VARCHAR
    FROM thirteenf_position_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'thirteenf_position_metrics', 'max_available_at', max(available_at)::VARCHAR
    FROM thirteenf_position_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'thirteenf_option_metrics', 'max_report_period', max(report_period)::VARCHAR
    FROM thirteenf_option_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'thirteenf_option_metrics', 'max_available_at', max(available_at)::VARCHAR
    FROM thirteenf_option_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'thirteenf_concentration_metrics', 'max_report_period', max(report_period)::VARCHAR
    FROM thirteenf_concentration_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'thirteenf_concentration_metrics', 'max_available_at', max(available_at)::VARCHAR
    FROM thirteenf_concentration_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'corporate_action_dividend_metrics', 'max_ex_date', max(ex_date)::VARCHAR
    FROM corporate_action_dividend_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'corporate_action_dividend_metrics', 'max_available_at', max(available_at)::VARCHAR
    FROM corporate_action_dividend_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'corporate_action_split_metrics', 'max_ex_date', max(ex_date)::VARCHAR
    FROM corporate_action_split_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'corporate_action_split_metrics', 'max_available_at', max(available_at)::VARCHAR
    FROM corporate_action_split_metrics
    HAVING count(*) > 0
    """,
    """
    SELECT 'corporate_action_factor_reconciliation', 'max_ex_date', max(ex_date)::VARCHAR
    FROM corporate_action_factor_reconciliation
    HAVING count(*) > 0
    """,
    """
    SELECT 'corporate_action_factor_reconciliation', 'max_available_at', max(available_at)::VARCHAR
    FROM corporate_action_factor_reconciliation
    HAVING count(*) > 0
    """,
    """
    SELECT 'sec_submissions', 'max_filing_date', max(filing_date)::VARCHAR
    FROM sec_submissions
    HAVING count(*) > 0
    """,
    """
    SELECT 'nasdaq_symbol_directory', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM nasdaq_symbol_directory
    HAVING count(*) > 0
    """,
    """
    SELECT 'nasdaq_listing_events', 'max_effective_date', max(effective_date)::VARCHAR
    FROM nasdaq_listing_events
    HAVING count(*) > 0
    """,
    """
    SELECT 'nasdaq_listing_events', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM nasdaq_listing_events
    HAVING count(*) > 0
    """,
    """
    SELECT 'nasdaq_listing_events', 'max_source_file_created_at', max(source_file_created_at)::VARCHAR
    FROM nasdaq_listing_events
    HAVING count(*) > 0
    """,
    """
    SELECT 'listing_status_intervals', 'max_valid_from', max(valid_from)::VARCHAR
    FROM listing_status_intervals
    HAVING count(*) > 0
    """,
    """
    SELECT 'listing_status_intervals', 'max_available_at', max(available_at)::VARCHAR
    FROM listing_status_intervals
    HAVING count(*) > 0
    """,
    """
    SELECT 'listing_status_intervals', 'max_last_evidence_as_of_date', max(last_evidence_as_of_date)::VARCHAR
    FROM listing_status_intervals
    HAVING count(*) > 0
    """,
    """
    SELECT 'delist_code_dim', 'max_updated_at', max(updated_at)::VARCHAR
    FROM delist_code_dim
    HAVING count(*) > 0
    """,
    """
    SELECT 'delisting_events', 'max_delist_date', max(delist_date)::VARCHAR
    FROM delisting_events
    HAVING count(*) > 0
    """,
    """
    SELECT 'delisting_events', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM delisting_events
    HAVING count(*) > 0
    """,
    """
    SELECT 'delisting_events', 'max_available_at', max(available_at)::VARCHAR
    FROM delisting_events
    HAVING count(*) > 0
    """,
    """
    SELECT 'delisting_return_observations', 'max_delist_date', max(delist_date)::VARCHAR
    FROM delisting_return_observations
    HAVING count(*) > 0
    """,
    """
    SELECT 'delisting_return_observations', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM delisting_return_observations
    HAVING count(*) > 0
    """,
    """
    SELECT 'delisting_return_observations', 'max_available_at', max(available_at)::VARCHAR
    FROM delisting_return_observations
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_detail', 'max_period_end', max(period_end)::VARCHAR
    FROM est_detail
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_detail', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM est_detail
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_detail', 'max_available_at', max(available_at)::VARCHAR
    FROM est_detail
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_actual', 'max_period_end', max(period_end)::VARCHAR
    FROM est_actual
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_actual', 'max_available_at', max(available_at)::VARCHAR
    FROM est_actual
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_surprise', 'max_period_end', max(period_end)::VARCHAR
    FROM est_surprise
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_surprise', 'max_available_at', max(available_at)::VARCHAR
    FROM est_surprise
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_consensus', 'max_consensus_date', max(consensus_date)::VARCHAR
    FROM est_consensus
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_consensus', 'max_period_end', max(period_end)::VARCHAR
    FROM est_consensus
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_consensus', 'max_available_at', max(available_at)::VARCHAR
    FROM est_consensus
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_guidance', 'max_guidance_date', max(guidance_date)::VARCHAR
    FROM est_guidance
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_guidance', 'max_available_at', max(available_at)::VARCHAR
    FROM est_guidance
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_guidance', 'sec_8k_extracted_rows', count(*)::VARCHAR
    FROM est_guidance
    WHERE source_item LIKE '8-K%'
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_recommendation', 'max_rating_date', max(rating_date)::VARCHAR
    FROM est_recommendation
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_recommendation', 'max_price_target_date', max(rating_date)::VARCHAR
    FROM est_recommendation
    WHERE price_target IS NOT NULL
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_recommendation', 'max_available_at', max(available_at)::VARCHAR
    FROM est_recommendation
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_recommendation_summary', 'max_snapshot_date', max(snapshot_date)::VARCHAR
    FROM est_recommendation_summary
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_recommendation_summary', 'max_price_target_snapshot_date', max(snapshot_date)::VARCHAR
    FROM est_recommendation_summary
    WHERE mean_price_target IS NOT NULL
       OR median_price_target IS NOT NULL
       OR price_target_count IS NOT NULL
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_recommendation_summary', 'max_available_at', max(available_at)::VARCHAR
    FROM est_recommendation_summary
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_security_link', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM est_security_link
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_security_link', 'max_available_at', max(available_at)::VARCHAR
    FROM est_security_link
    HAVING count(*) > 0
    """,
    """
    SELECT 'est_security_link', 'accepted_links', count(*)::VARCHAR
    FROM est_security_link
    WHERE link_status = 'accepted'
    HAVING count(*) > 0
    """,
    """
    SELECT 'fred_macro', 'max_observation_date', max(observation_date)::VARCHAR
    FROM macro_observations
    HAVING count(*) > 0
    """,
    """
    SELECT 'fred_macro', 'max_observation_date:' || series_id, max(observation_date)::VARCHAR
    FROM macro_observations
    GROUP BY series_id
    """,
    """
    SELECT 'trading_calendar', 'max_trade_date', max(trade_date)::VARCHAR
    FROM trading_calendar
    HAVING count(*) > 0
    """,
    """
    SELECT 'universe_memberships', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM universe_memberships
    HAVING count(*) > 0
    """,
    """
    SELECT 'universe_memberships', 'max_as_of_date:' || universe_id, max(as_of_date)::VARCHAR
    FROM universe_memberships
    GROUP BY universe_id
    """,
    """
    SELECT
        CASE
            WHEN feature_set = 'equity_daily_v1' THEN 'equity_daily_features'
            WHEN feature_set = 'sec_fundamentals_v1' THEN 'sec_fundamental_features'
            WHEN feature_set = 'sec_13f_ownership_v1' THEN 'sec_13f_ownership_features'
            WHEN feature_set = 'finra_short_interest_v1' THEN 'finra_short_interest_features'
            ELSE 'feature_values'
        END,
        'max_as_of_date:' || feature_set,
        max(as_of_date)::VARCHAR
    FROM feature_values
    GROUP BY feature_set
    """,
    """
    SELECT 'feature_lineage', 'max_updated_at', max(updated_at)::VARCHAR
    FROM feature_dependency_edges
    HAVING count(*) > 0
    """,
    """
    SELECT 'alpha_research', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM alpha_signal_values
    HAVING count(*) > 0
    """,
    """
    SELECT 'alpha_research', 'max_available_at', max(available_at)::VARCHAR
    FROM alpha_signal_values
    HAVING count(*) > 0
    """,
    """
    SELECT 'alpha_research', 'max_backtest_end_date', max(end_date)::VARCHAR
    FROM alpha_backtest_manifests
    HAVING count(*) > 0
    """,
    """
    SELECT 'identifier_resolution_candidates', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM identifier_resolution_candidates
    HAVING count(*) > 0
    """,
    """
    SELECT 'identifier_resolution_decisions', 'max_as_of_date', max(as_of_date)::VARCHAR
    FROM identifier_resolution_decisions
    HAVING count(*) > 0
    """,
    """
    SELECT 'provider_parity_matrix', 'max_updated_at', max(updated_at)::VARCHAR
    FROM provider_parity_matrix
    HAVING count(*) > 0
    """,
    """
    SELECT 'warehouse_lake_exports', 'last_succeeded_export', max(finished_at)::VARCHAR
    FROM lake_export_runs
    WHERE status = 'succeeded'
    HAVING count(*) > 0
    """,
)


@dataclass(frozen=True)
class WatermarkRefreshResult:
    rows_upserted: int
    watermarks: list[dict[str, str]]


def _watermark_rows(store: DuckDBStore) -> Iterable[tuple[str, str, str]]:
    for sql in WATERMARK_QUERIES:
        try:
            rows = store.con.execute(sql).fetchall()
        except duckdb.CatalogException as exc:
            if "does not exist" in str(exc):
                continue
            raise
        for dataset_id, watermark_name, watermark_value in rows:
            if dataset_id and watermark_name and watermark_value is not None:
                yield str(dataset_id), str(watermark_name), str(watermark_value)


def refresh_warehouse_watermarks(store: DuckDBStore) -> WatermarkRefreshResult:
    store.initialize()
    rows = sorted(set(_watermark_rows(store)))
    for dataset_id, watermark_name, watermark_value in rows:
        store.con.execute(
            """
            DELETE FROM dataset_watermarks
            WHERE dataset_id = ? AND watermark_name = ?
            """,
            [dataset_id, watermark_name],
        )
        store.con.execute(
            """
            INSERT INTO dataset_watermarks (
                dataset_id,
                watermark_name,
                watermark_value,
                updated_at
            )
            VALUES (?, ?, ?, now())
            """,
            [dataset_id, watermark_name, watermark_value],
        )
    return WatermarkRefreshResult(
        rows_upserted=len(rows),
        watermarks=[
            {
                "dataset_id": dataset_id,
                "watermark_name": watermark_name,
                "watermark_value": watermark_value,
            }
            for dataset_id, watermark_name, watermark_value in rows
        ],
    )
