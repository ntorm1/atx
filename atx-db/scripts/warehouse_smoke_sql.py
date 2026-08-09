#!/usr/bin/env python
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import duckdb

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH


SMOKE_SQL = [
    ("tables", "SELECT table_name, estimated_size FROM duckdb_tables() WHERE schema_name='main' ORDER BY table_name"),
    (
        "duplicate_daily_bars",
        """
        SELECT source, security_id, trade_date, count(*) AS rows
        FROM equity_daily_bars
        GROUP BY 1,2,3
        HAVING count(*) > 1
        """,
    ),
    (
        "bad_daily_bars",
        """
        SELECT *
        FROM equity_daily_bars
        WHERE volume < 0
           OR open <= 0
           OR high <= 0
           OR low <= 0
           OR close <= 0
           OR high < greatest(open, low, close)
           OR low > least(open, high, close)
        LIMIT 20
        """,
    ),
    (
        "aapl_panel_tail",
        """
        SELECT *
        FROM v_alpha_daily_panel
        WHERE symbol='AAPL'
        ORDER BY as_of_date DESC
        LIMIT 10
        """,
    ),
    (
        "nasdaq_listing_events",
        """
        SELECT
            coalesce(nasdaq_action, '') AS nasdaq_action,
            coalesce(bx_action, '') AS bx_action,
            coalesce(psx_action, '') AS psx_action,
            primary_listing_market,
            count(*) AS event_rows,
            count(security_id) AS resolved_security_ids,
            min(effective_date) AS min_effective_date,
            max(effective_date) AS max_effective_date,
            max(source_file_created_at) AS latest_source_file_created_at
        FROM nasdaq_listing_events
        GROUP BY 1, 2, 3, 4
        ORDER BY event_rows DESC, nasdaq_action, bx_action, psx_action, primary_listing_market
        """,
    ),
    (
        "listing_status_intervals",
        """
        SELECT
            evidence_source,
            method,
            status,
            count(*) AS interval_rows,
            count(DISTINCT symbol) AS symbols,
            count(security_id) AS resolved_security_rows,
            min(valid_from) AS min_valid_from,
            max(valid_from) AS max_valid_from,
            max(available_at) AS max_available_at
        FROM listing_status_intervals
        GROUP BY 1, 2, 3
        ORDER BY evidence_source, method, status
        """,
    ),
    (
        "features",
        """
        SELECT feature_set, feature_name, count(*) AS rows, min(as_of_date) AS min_date, max(as_of_date) AS max_date
        FROM feature_values
        GROUP BY 1, 2
        ORDER BY 1, 2
        """,
    ),
    (
        "sec_fundamental_features",
        """
        SELECT
            feature_name,
            count(*) AS rows,
            min(as_of_date) AS min_date,
            max(as_of_date) AS max_date,
            min(available_at) AS min_available_at,
            max(available_at) AS max_available_at
        FROM feature_values
        WHERE feature_set = 'sec_fundamentals_v1'
        GROUP BY 1
        ORDER BY 1
        """,
    ),
    (
        "finra_short_interest_features",
        """
        SELECT
            feature_name,
            count(*) AS rows,
            min(as_of_date) AS min_as_of_date,
            max(as_of_date) AS max_as_of_date,
            min(available_at) AS min_available_at,
            max(available_at) AS max_available_at
        FROM feature_values
        WHERE feature_set = 'finra_short_interest_v1'
        GROUP BY 1
        ORDER BY 1
        """,
    ),
    (
        "alpha_expression_catalog",
        """
        SELECT
            alpha_id,
            alpha_name,
            feature_set,
            universe_id,
            horizon_days,
            input_features_json
        FROM alpha_expression_catalog
        ORDER BY alpha_id
        """,
    ),
    (
        "alpha_signal_values",
        """
        SELECT
            alpha_id,
            count(*) AS signals,
            count(DISTINCT security_id) AS securities,
            min(as_of_date) AS min_as_of_date,
            max(as_of_date) AS max_as_of_date,
            min(available_at) AS min_available_at,
            max(available_at) AS max_available_at,
            avg(abs(weight)) AS avg_abs_weight
        FROM alpha_signal_values
        GROUP BY 1
        ORDER BY 1
        """,
    ),
    (
        "alpha_backtest_manifests",
        """
        SELECT
            alpha_id,
            start_date,
            end_date,
            evaluation_days,
            signal_count,
            security_count,
            mean_daily_long_short_return,
            mean_rank_ic,
            hit_rate,
            cumulative_long_short_return
        FROM alpha_backtest_manifests
        ORDER BY alpha_id
        """,
    ),
    (
        "xbrl_concept_catalog",
        """
        SELECT
            taxonomy,
            statement_category,
            count(*) AS concepts,
            sum(fact_count) AS fact_rows,
            min(first_filed_date) AS first_filed_date,
            max(last_filed_date) AS last_filed_date,
            max(last_available_at) AS last_available_at
        FROM xbrl_concept_catalog
        GROUP BY 1, 2
        ORDER BY taxonomy, statement_category
        """,
    ),
    (
        "xbrl_taxonomy_packages",
        """
        SELECT
            taxonomy_package_id,
            taxonomy,
            release_year,
            file_count,
            linkbase_file_count,
            relationship_count,
            byte_count,
            source_loaded_at
        FROM xbrl_taxonomy_packages
        ORDER BY taxonomy_package_id
        """,
    ),
    (
        "xbrl_taxonomy_relationships",
        """
        SELECT
            taxonomy_package_id,
            linkbase_type,
            count(*) AS relationships,
            count(DISTINCT role_uri) AS roles,
            count(DISTINCT source_file) AS source_files,
            sum(CASE WHEN touches_observed_concept THEN 1 ELSE 0 END) AS observed_concept_relationships
        FROM xbrl_taxonomy_relationships
        GROUP BY 1, 2
        ORDER BY taxonomy_package_id, linkbase_type
        """,
    ),
    (
        "xbrl_dimension_edges",
        """
        SELECT
            relationship_kind,
            context_element,
            count(*) AS edges,
            count(DISTINCT source_concept) AS source_concepts,
            count(DISTINCT target_concept) AS target_concepts,
            sum(CASE WHEN touches_observed_concept THEN 1 ELSE 0 END) AS observed_concept_edges
        FROM xbrl_dimension_edges
        GROUP BY 1, 2
        ORDER BY edges DESC, relationship_kind, context_element
        LIMIT 25
        """,
    ),
    (
        "xbrl_fact_frames",
        """
        SELECT
            taxonomy,
            concept,
            frame_period,
            count(*) AS frames,
            sum(fact_count) AS facts,
            min(frame_year) AS first_frame_year,
            max(frame_year) AS last_frame_year,
            max(last_available_at) AS latest_available_at
        FROM xbrl_fact_frames
        GROUP BY 1, 2, 3
        ORDER BY taxonomy, concept, frame_period
        LIMIT 50
        """,
    ),
    (
        "xbrl_filing_contexts",
        """
        SELECT
            form,
            period_type,
            count(*) AS contexts,
            sum(dimension_count) AS dimension_members,
            count(DISTINCT accession_number) AS filings,
            min(coalesce(period_start, instant_date)) AS first_period_date,
            max(coalesce(period_end, instant_date)) AS last_period_date,
            max(acceptance_datetime) AS latest_acceptance
        FROM xbrl_filing_contexts
        GROUP BY 1, 2
        ORDER BY contexts DESC, form, period_type
        LIMIT 25
        """,
    ),
    (
        "xbrl_filing_dimensions",
        """
        SELECT
            context_element,
            member_kind,
            dimension_qname,
            member_qname,
            count(*) AS rows,
            count(DISTINCT accession_number) AS filings,
            max(acceptance_datetime) AS latest_acceptance
        FROM xbrl_filing_dimensions
        GROUP BY 1, 2, 3, 4
        ORDER BY rows DESC, dimension_qname, member_qname
        LIMIT 50
        """,
    ),
    (
        "xbrl_filing_facts",
        """
        SELECT
            fact_kind,
            taxonomy,
            concept,
            count(*) AS facts,
            count(DISTINCT accession_number) AS filings,
            count(DISTINCT context_ref) AS contexts,
            sum(CASE WHEN is_numeric THEN 1 ELSE 0 END) AS numeric_facts,
            max(acceptance_datetime) AS latest_acceptance
        FROM xbrl_filing_facts
        GROUP BY 1, 2, 3
        ORDER BY facts DESC, taxonomy, concept
        LIMIT 50
        """,
    ),
    (
        "fundamental_fact_revisions",
        """
        WITH groups AS (
            SELECT
                taxonomy,
                concept,
                revision_group_id,
                max(revision_count) AS revision_count,
                sum(CASE WHEN is_value_changed THEN 1 ELSE 0 END) AS value_change_rows,
                max(latest_available_at) AS latest_available_at
            FROM fundamental_fact_revisions
            GROUP BY 1, 2, 3
        )
        SELECT
            taxonomy,
            concept,
            count(*) AS fact_groups,
            sum(revision_count) AS revision_rows,
            sum(CASE WHEN revision_count > 1 THEN 1 ELSE 0 END) AS revised_groups,
            sum(value_change_rows) AS value_change_rows,
            max(latest_available_at) AS latest_available_at
        FROM groups
        GROUP BY 1, 2
        ORDER BY revised_groups DESC, value_change_rows DESC, taxonomy, concept
        LIMIT 25
        """,
    ),
    (
        "fundamental_statement_map",
        """
        SELECT
            statement_type,
            statement_section,
            canonical_metric,
            taxonomy,
            concept,
            period_type,
            unit_type,
            concept_priority,
            is_active
        FROM fundamental_statement_map
        ORDER BY statement_type, statement_section, canonical_metric, concept_priority, taxonomy, concept
        """,
    ),
    (
        "fundamental_statement_points",
        """
        SELECT
            statement_type,
            canonical_metric,
            count(*) AS rows,
            count(DISTINCT revision_group_id) AS fact_groups,
            count(DISTINCT security_id) AS securities,
            sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows,
            min(as_of_date) AS first_as_of_date,
            max(as_of_date) AS latest_as_of_date,
            max(available_at) AS latest_available_at
        FROM fundamental_statement_points
        GROUP BY 1, 2
        ORDER BY statement_type, canonical_metric
        """,
    ),
    (
        "fundamental_ttm_points",
        """
        SELECT
            statement_type,
            canonical_metric,
            count(*) AS rows,
            count(DISTINCT ttm_revision_group_id) AS ttm_groups,
            count(DISTINCT security_id) AS securities,
            sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows,
            min(ttm_start_date) AS first_ttm_start,
            max(ttm_end_date) AS latest_ttm_end,
            min(coverage_days) AS min_coverage_days,
            max(coverage_days) AS max_coverage_days,
            max(available_at) AS latest_available_at
        FROM fundamental_ttm_points
        GROUP BY 1, 2
        ORDER BY statement_type, canonical_metric
        """,
    ),
    (
        "fundamental_periods",
        """
        SELECT
            normalized_period_type,
            count(*) AS rows,
            count(DISTINCT period_group_id) AS period_groups,
            count(DISTINCT security_id) AS securities,
            sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows,
            min(period_start) AS first_period_start,
            max(period_end) AS latest_period_end,
            min(period_days) AS min_period_days,
            max(period_days) AS max_period_days,
            max(available_at) AS latest_available_at
        FROM fundamental_periods
        GROUP BY 1
        ORDER BY normalized_period_type
        """,
    ),
    (
        "feature_definitions",
        """
        SELECT feature_set, feature_name, lookback_days, is_point_in_time_safe, available_at_policy
        FROM feature_definitions
        ORDER BY feature_set, feature_name
        """,
    ),
    (
        "feature_set_catalog",
        """
        SELECT
            feature_set,
            version_label,
            feature_family,
            feature_count,
            dependency_count,
            source_table_count,
            derived_feature_dependency_count,
            max_lookback_days,
            point_in_time_safe
        FROM feature_set_catalog
        ORDER BY feature_set
        """,
    ),
    (
        "feature_dependency_edges",
        """
        SELECT
            feature_set,
            dependency_type,
            dependency_name,
            count(*) AS edge_count,
            count(DISTINCT feature_name) AS dependent_features
        FROM feature_dependency_edges
        GROUP BY 1, 2, 3
        ORDER BY feature_set, dependency_type, dependency_name
        """,
    ),
    (
        "feature_build_manifests",
        """
        SELECT
            feature_set,
            run_id,
            symbols_json,
            feature_count,
            input_row_count,
            output_row_count,
            input_min_as_of_date,
            input_max_as_of_date,
            output_min_as_of_date,
            output_max_as_of_date,
            max_available_at
        FROM feature_build_manifests
        ORDER BY source_loaded_at DESC
        LIMIT 10
        """,
    ),
    (
        "corporate_actions",
        """
        SELECT
            action_type,
            symbol,
            count(*) AS rows,
            min(ex_date) AS min_ex_date,
            max(ex_date) AS max_ex_date,
            sum(cash_amount) AS total_cash_amount
        FROM corporate_actions
        GROUP BY action_type, symbol
        ORDER BY action_type, symbol
        """,
    ),
    (
        "catalog",
        """
        SELECT dataset_id, source_system_id, primary_table, grain
        FROM dataset_catalog
        ORDER BY dataset_id
        """,
    ),
    (
        "provider_parity_matrix",
        """
        SELECT
            provider,
            provider_domain,
            warehouse_domain,
            parity_status,
            next_gap
        FROM provider_parity_matrix
        ORDER BY provider, provider_domain
        """,
    ),
    (
        "sec_submissions",
        """
        SELECT form, count(*) AS rows, max(filing_date) AS latest_filing_date
        FROM sec_submissions
        GROUP BY 1
        ORDER BY rows DESC
        LIMIT 20
        """,
    ),
    (
        "thirteenf_identifier_coverage",
        """
        WITH holdings AS (
            SELECT accession_number, infotable_sk, source_period, cusip, security_id
            FROM thirteenf_holdings
        ),
        identifier_cusips AS (
            SELECT DISTINCT id_value
            FROM security_identifier_history
            WHERE id_type = 'CUSIP'
        )
        SELECT
            count(*) AS holding_rows,
            count(h.security_id) AS mapped_holding_rows,
            count(DISTINCT cusip) AS holding_cusips,
            count(DISTINCT h.security_id) AS mapped_securities,
            count(DISTINCT CASE WHEN i.id_value IS NOT NULL THEN h.cusip END) AS cusips_with_identifier_history
        FROM holdings h
        LEFT JOIN identifier_cusips i
          ON i.id_value = h.cusip
        """,
    ),
    (
        "thirteenf_ownership_features",
        """
        SELECT
            source_period,
            count(*) AS ownership_rows,
            count(DISTINCT security_id) AS securities,
            count(DISTINCT cusip) AS cusips,
            sum(holding_row_count) AS holding_rows,
            sum(common_value_usd) AS common_value_usd,
            sum(common_share_quantity) AS common_share_quantity,
            max(available_at) AS max_available_at
        FROM thirteenf_security_ownership
        GROUP BY 1
        ORDER BY source_period DESC
        """,
    ),
    (
        "thirteenf_top_managers",
        """
        SELECT
            manager_id,
            manager_name,
            filing_count,
            first_report_period,
            last_report_period
        FROM thirteenf_managers
        ORDER BY filing_count DESC, manager_id
        LIMIT 20
        """,
    ),
    (
        "identifier_resolution_candidates",
        """
        SELECT
            candidate_status,
            count(*) AS rows,
            count(DISTINCT source_key_value) AS source_keys,
            count(DISTINCT target_security_id) AS target_securities,
            avg(confidence) AS avg_confidence,
            max(confidence) AS max_confidence
        FROM identifier_resolution_candidates
        GROUP BY candidate_status
        ORDER BY candidate_status
        """,
    ),
    (
        "identifier_resolution_decisions",
        """
        SELECT
            decision_status,
            candidate_status,
            count(*) AS rows,
            count(DISTINCT source_key_value) AS source_keys,
            count(DISTINCT target_security_id) AS target_securities,
            avg(confidence) AS avg_confidence,
            max(decided_at) AS latest_decided_at
        FROM identifier_resolution_decisions
        GROUP BY decision_status, candidate_status
        ORDER BY decision_status, candidate_status
        """,
    ),
    (
        "macro_latest",
        """
        SELECT series_id, max(observation_date) AS latest_observation_date, count(*) AS rows
        FROM macro_observations
        GROUP BY 1
        ORDER BY series_id
        """,
    ),
    (
        "macro_series",
        """
        SELECT source, series_id, title, frequency, units
        FROM macro_series
        ORDER BY source, series_id
        """,
    ),
    (
        "universes",
        """
        SELECT
            u.universe_id,
            u.name,
            count(m.security_id) AS membership_rows,
            min(m.as_of_date) AS min_as_of_date,
            max(m.as_of_date) AS max_as_of_date
        FROM universes u
        LEFT JOIN universe_memberships m
          ON m.universe_id = u.universe_id
        GROUP BY u.universe_id, u.name
        ORDER BY u.universe_id
        """,
    ),
    (
        "etl_retry_policies",
        """
        SELECT
            job_name,
            dataset_id,
            enabled,
            max_retries,
            retry_delay_seconds,
            dependencies_json
        FROM etl_job_definitions
        ORDER BY job_name
        """,
    ),
    (
        "lake_export_runs",
        """
        SELECT
            r.export_run_id,
            r.status,
            r.object_count,
            count(f.object_name) AS file_count,
            r.total_rows,
            r.total_byte_count,
            r.started_at,
            r.finished_at
        FROM lake_export_runs r
        LEFT JOIN lake_export_files f
          ON f.export_run_id = r.export_run_id
        GROUP BY
            r.export_run_id,
            r.status,
            r.object_count,
            r.total_rows,
            r.total_byte_count,
            r.started_at,
            r.finished_at
        ORDER BY r.started_at DESC
        LIMIT 10
        """,
    ),
    (
        "dataset_watermarks",
        """
        SELECT dataset_id, watermark_name, watermark_value, updated_at
        FROM dataset_watermarks
        ORDER BY dataset_id, watermark_name
        """,
    ),
    (
        "job_runs",
        """
        SELECT
            job_name,
            dataset_id,
            status,
            attempt_count,
            max_retries,
            retry_delay_seconds,
            rows_loaded,
            started_at,
            finished_at
        FROM etl_job_runs
        ORDER BY started_at DESC
        LIMIT 20
        """,
    ),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run warehouse smoke SQL checks.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    con = duckdb.connect(str(args.db_path), read_only=True)
    try:
        for name, sql in SMOKE_SQL:
            print(f"\n-- {name}")
            print(con.execute(sql).fetchdf().to_string(index=False))
    finally:
        con.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
