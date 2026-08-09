from __future__ import annotations

from ._check_common import DEFAULT_EXPORT_OBJECTS, _export_scan_internal_cusip_sql
from ._types import SqlQualityCheck


def ownership_check_specs(
    *,
    daily_macro_stale_days: int,
    monthly_macro_stale_days: int,
    valuation_stale_gap_days: int,
) -> tuple[SqlQualityCheck, ...]:
    return (
        SqlQualityCheck(
            dataset_id="finra_short_interest",
            table_name="finra_short_interest",
            check_name="bad_finra_short_quantities",
            sql="""
                SELECT count(*)::DOUBLE
                FROM finra_short_interest
                WHERE current_short_position_quantity < 0
                   OR previous_short_position_quantity < 0
                   OR average_daily_volume_quantity < 0
                   OR days_to_cover_quantity < 0
            """,
            threshold=0.0,
            required_tables=("finra_short_interest",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest",
            table_name="finra_short_interest",
            check_name="duplicate_finra_short_interest",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        settlement_date,
                        symbol,
                        coalesce(market_class_code, '') AS market_class_code,
                        count(*) AS row_count
                    FROM finra_short_interest
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("finra_short_interest",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest_backfills",
            table_name="finra_short_interest_backfill_manifests",
            check_name="bad_finra_short_interest_backfill_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM finra_short_interest_backfill_manifests
                WHERE manifest_id IS NULL
                   OR manifest_id = ''
                   OR status NOT IN ('succeeded', 'skipped')
                   OR start_date IS NULL
                   OR end_date IS NULL
                   OR start_date > end_date
                   OR date_order NOT IN ('asc', 'desc')
                   OR limit_dates < 1
                   OR (skip_existing_min_symbols IS NOT NULL AND skip_existing_min_symbols < 1)
                   OR candidate_count < 0
                   OR selected_date_count < 0
                   OR loaded_date_count < 0
                   OR source_row_count < 0
                   OR (feature_row_count IS NOT NULL AND feature_row_count < 0)
                   OR selected_dates_json IS NULL
                   OR selected_dates_json = ''
                   OR candidates_json IS NULL
                   OR candidates_json = ''
                   OR load_results_json IS NULL
                   OR load_results_json = ''
                   OR source IS NULL
                   OR source = ''
                   OR started_at IS NULL
                   OR finished_at IS NULL
                   OR finished_at < started_at
            """,
            threshold=0.0,
            required_tables=("finra_short_interest_backfill_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest_backfills",
            table_name="finra_short_interest_backfill_manifests",
            check_name="inconsistent_finra_short_interest_backfill_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM finra_short_interest_backfill_manifests
                WHERE (status = 'succeeded' AND loaded_date_count <> selected_date_count)
                   OR (status = 'succeeded' AND selected_date_count = 0)
                   OR (status = 'skipped' AND loaded_date_count <> 0)
                   OR selected_date_count > candidate_count
            """,
            threshold=0.0,
            required_tables=("finra_short_interest_backfill_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest_features",
            table_name="feature_values",
            check_name="missing_finra_short_interest_feature_values",
            sql="""
                WITH expected AS (
                    SELECT DISTINCT security_id, settlement_date AS as_of_date
                    FROM finra_short_interest
                    WHERE security_id IS NOT NULL
                      AND security_id <> ''
                      AND settlement_date IS NOT NULL
                )
                SELECT count(*)::DOUBLE
                FROM expected e
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM feature_values f
                    WHERE f.feature_set = 'finra_short_interest_v1'
                      AND f.feature_name = 'si_current_short_position'
                      AND f.security_id = e.security_id
                      AND f.as_of_date = e.as_of_date
                )
            """,
            threshold=0.0,
            required_tables=("finra_short_interest", "feature_values"),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest_features",
            table_name="feature_values",
            check_name="missing_finra_short_interest_xsec_features",
            sql="""
                WITH panel AS (
                    SELECT
                        security_id,
                        settlement_date AS as_of_date,
                        sum(coalesce(current_short_position_quantity, 0))::DOUBLE AS current_short_position_quantity,
                        sum(coalesce(average_daily_volume_quantity, 0))::DOUBLE AS average_daily_volume_quantity
                    FROM finra_short_interest
                    WHERE security_id IS NOT NULL
                      AND security_id <> ''
                      AND settlement_date IS NOT NULL
                    GROUP BY 1, 2
                ),
                eligible AS (
                    SELECT
                        security_id,
                        as_of_date,
                        count(*) OVER (PARTITION BY as_of_date) AS eligible_security_count
                    FROM panel
                    WHERE average_daily_volume_quantity > 0
                      AND current_short_position_quantity IS NOT NULL
                )
                SELECT count(*)::DOUBLE
                FROM eligible e
                WHERE e.eligible_security_count >= 20
                  AND NOT EXISTS (
                      SELECT 1
                      FROM feature_values f
                      WHERE f.feature_set = 'finra_short_interest_v1'
                        AND f.feature_name = 'si_short_to_adv_xsec_percentile'
                        AND f.security_id = e.security_id
                        AND f.as_of_date = e.as_of_date
                  )
            """,
            threshold=0.0,
            required_tables=("finra_short_interest", "feature_values"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f",
            table_name="thirteenf_holdings",
            check_name="bad_13f_holding_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_holdings
                WHERE accession_number IS NULL
                   OR accession_number = ''
                   OR cusip IS NULL
                   OR cusip = ''
                   OR source_period IS NULL
                   OR source_period = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_holdings",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f",
            table_name="thirteenf_holdings",
            check_name="duplicate_13f_holdings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT accession_number, infotable_sk, source_period, count(*) AS row_count
                    FROM thirteenf_holdings
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_holdings",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f",
            table_name="thirteenf_holdings",
            check_name="missing_13f_security_ids",
            sql="SELECT count(*)::DOUBLE FROM thirteenf_holdings WHERE security_id IS NULL OR security_id = ''",
            threshold=0.0,
            required_tables=("thirteenf_holdings",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f",
            table_name="security_identifier_history",
            check_name="thirteenf_cusips_without_identifier_history",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT cusip
                    FROM thirteenf_holdings
                ) h
                LEFT JOIN security_identifier_history i
                  ON i.id_type = 'CUSIP'
                 AND i.id_value = h.cusip
                WHERE i.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("thirteenf_holdings", "security_identifier_history"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_managers",
            check_name="duplicate_13f_manager_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT manager_id, count(*) AS row_count
                    FROM thirteenf_managers
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_managers",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_manager_reports",
            check_name="duplicate_13f_manager_report_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT manager_report_id, count(*) AS row_count
                    FROM thirteenf_manager_reports
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_manager_reports",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_positions",
            check_name="duplicate_13f_position_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT position_id, count(*) AS row_count
                    FROM thirteenf_security_positions
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_positions",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_ownership",
            check_name="duplicate_13f_ownership_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT ownership_id, count(*) AS row_count
                    FROM thirteenf_security_ownership
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_ownership",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_managers",
            check_name="bad_13f_manager_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_managers
                WHERE manager_id IS NULL
                   OR manager_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR filing_count < 1
                   OR amendment_count < 0
                   OR source_period_count < 1
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_managers",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_manager_reports",
            check_name="bad_13f_manager_report_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_manager_reports
                WHERE manager_report_id IS NULL
                   OR manager_report_id = ''
                   OR manager_id IS NULL
                   OR manager_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR report_period IS NULL
                   OR filing_date IS NULL
                   OR report_period > filing_date
                   OR source_period IS NULL
                   OR source_period = ''
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_manager_reports",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_positions",
            check_name="bad_13f_security_position_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_positions
                WHERE position_id IS NULL
                   OR position_id = ''
                   OR manager_report_id IS NULL
                   OR manager_report_id = ''
                   OR manager_id IS NULL
                   OR manager_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR cusip IS NULL
                   OR cusip = ''
                   OR report_period IS NULL
                   OR filing_date IS NULL
                   OR report_period > filing_date
                   OR as_of_date <> report_period
                   OR source_period IS NULL
                   OR source_period = ''
                   OR coalesce(value_usd, 0) < 0
                   OR coalesce(share_quantity, 0) < 0
                   OR coalesce(portfolio_weight, 0) < 0
                   OR portfolio_weight > 1.000001
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_positions",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_ownership",
            check_name="bad_13f_security_ownership_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_ownership
                WHERE ownership_id IS NULL
                   OR ownership_id = ''
                   OR cusip IS NULL
                   OR cusip = ''
                   OR report_period IS NULL
                   OR as_of_date <> report_period
                   OR source_period IS NULL
                   OR source_period = ''
                   OR holding_row_count < 1
                   OR filing_count < 1
                   OR holder_count < 1
                   OR common_holder_count < 0
                   OR coalesce(common_value_usd, 0) < 0
                   OR coalesce(common_share_quantity, 0) < 0
                   OR coalesce(call_share_quantity, 0) < 0
                   OR coalesce(put_share_quantity, 0) < 0
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_ownership",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_manager_reports",
            check_name="orphan_13f_manager_reports",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_manager_reports r
                LEFT JOIN thirteenf_managers m
                  ON m.manager_id = r.manager_id
                WHERE m.manager_id IS NULL
            """,
            threshold=0.0,
            required_tables=("thirteenf_manager_reports", "thirteenf_managers"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_positions",
            check_name="orphan_13f_security_positions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_positions p
                LEFT JOIN thirteenf_manager_reports r
                  ON r.manager_report_id = p.manager_report_id
                WHERE r.manager_report_id IS NULL
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_positions", "thirteenf_manager_reports"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_ownership",
            check_name="orphan_13f_ownership_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_ownership o
                LEFT JOIN securities s
                  ON s.security_id = o.security_id
                WHERE o.security_id IS NOT NULL
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_ownership", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="feature_values",
            check_name="missing_13f_ownership_feature_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_ownership o
                WHERE o.security_id IS NOT NULL
                  AND NOT EXISTS (
                      SELECT 1
                      FROM feature_values f
                      WHERE f.feature_set = 'sec_13f_ownership_v1'
                        AND f.feature_name = 'own_13f_holder_count'
                        AND f.security_id = o.security_id
                        AND f.as_of_date = o.as_of_date
                  )
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_ownership", "feature_values"),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="filing_form4",
            check_name="duplicate_form4_filings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT accession_number, count(*) AS row_count
                    FROM filing_form4
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("filing_form4",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="filing_form4",
            check_name="bad_form4_filing_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM filing_form4
                WHERE filing_id IS NULL
                   OR filing_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR form_type NOT IN ('3', '3/A', '4', '4/A', '5', '5/A')
                   OR available_at IS NULL
                   OR source_url IS NULL
                   OR source_url = ''
            """,
            threshold=0.0,
            required_tables=("filing_form4",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="insider_relationship",
            check_name="bad_insider_relationship_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_relationship
                WHERE relationship_id IS NULL
                   OR relationship_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR insider_id IS NULL
                   OR insider_id = ''
                   OR full_name IS NULL
                   OR full_name = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR valid_from IS NULL
                   OR (valid_to IS NOT NULL AND valid_to <= valid_from)
                   OR available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("insider_relationship",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="insider_transaction",
            check_name="duplicate_insider_transaction_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT transaction_id, count(*) AS row_count
                    FROM insider_transaction
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("insider_transaction",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="insider_transaction",
            check_name="bad_insider_transaction_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_transaction
                WHERE transaction_id IS NULL
                   OR transaction_id = ''
                   OR filing_id IS NULL
                   OR filing_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR insider_id IS NULL
                   OR insider_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR transaction_date IS NULL
                   OR transaction_code IS NULL
                   OR upper(transaction_code) NOT IN (
                        'P','S','A','D','F','I','M','G','L','W','Z','C','E','H',
                        'O','X','U','J','K','V','Q','R','B','T','N','Y','1','2'
                   )
                   OR coalesce(transaction_shares, 0) < 0
                   OR coalesce(shares_owned_following, 0) < 0
                   OR available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("insider_transaction",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="insider_transaction",
            check_name="orphan_insider_transactions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_transaction t
                LEFT JOIN filing_form4 f ON f.filing_id = t.filing_id
                LEFT JOIN insider i ON i.insider_id = t.insider_id
                WHERE f.filing_id IS NULL
                   OR i.insider_id IS NULL
            """,
            threshold=0.0,
            required_tables=("insider_transaction", "filing_form4", "insider"),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="tradingplan_10b5_1",
            check_name="bad_tradingplan_10b5_1_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM tradingplan_10b5_1
                WHERE plan_id IS NULL
                   OR plan_id = ''
                   OR insider_id IS NULL
                   OR insider_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR adoption_date IS NULL
                   OR transaction_count < 1
                   OR cooling_off_days < 0
                   OR available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("tradingplan_10b5_1",),
        ),
        SqlQualityCheck(
            dataset_id="insider_transaction_metrics",
            table_name="insider_transaction_metrics",
            check_name="insider_transaction_metrics_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, security_id, signal_date, window_days,
                           count(*) FILTER (WHERE is_latest_revision) AS latest_rows
                    FROM insider_transaction_metrics
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) FILTER (WHERE is_latest_revision) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("insider_transaction_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="insider_transaction_metrics",
            table_name="insider_transaction_metrics",
            check_name="bad_insider_transaction_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_transaction_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR signal_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR window_days < 1
                   OR cluster_min_buyers < 1
                   OR cluster_min_purchase_value < 0
                   OR transaction_count < 1
                   OR open_market_purchase_count < 0
                   OR open_market_sale_count < 0
                   OR discretionary_sale_count < 0
                   OR plan_sale_count < 0
                   OR unique_insider_count < 0
                   OR buyer_count < 0
                   OR seller_count < 0
                   OR cluster_purchase_count < 0
                   OR cluster_buyer_count < 0
                   OR cluster_sale_count < 0
                   OR cluster_seller_count < 0
                   OR coalesce(gross_purchase_shares, 0) < 0
                   OR coalesce(gross_sale_shares, 0) < 0
                   OR coalesce(gross_purchase_value, 0) < 0
                   OR coalesce(gross_sale_value, 0) < 0
                   OR coalesce(discretionary_sale_value, 0) < 0
                   OR coalesce(plan_sale_value, 0) < 0
                   OR coalesce(cluster_purchase_value, 0) < 0
                   OR coalesce(cluster_sale_value, 0) < 0
                   OR (plan_sale_value_ratio IS NOT NULL AND (plan_sale_value_ratio < 0 OR plan_sale_value_ratio > 1))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("insider_transaction_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="insider_transaction_metrics",
            table_name="insider_transaction_metrics",
            check_name="insider_transaction_metric_flags_consistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_transaction_metrics
                WHERE (is_cluster_buy AND (
                           cluster_buyer_count < cluster_min_buyers
                           OR coalesce(cluster_purchase_value, 0) < cluster_min_purchase_value
                       ))
                   OR (is_10b5_1_heavy_sale AND (
                           coalesce(gross_sale_value, 0) <= 0
                           OR coalesce(plan_sale_value_ratio, 0) < 0.5
                       ))
                   OR plan_sale_value > gross_sale_value
                   OR discretionary_sale_value > gross_sale_value
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("insider_transaction_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="security_listing_metrics",
            table_name="security_listing_metrics",
            check_name="security_listing_metrics_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, security_id, as_of_date,
                           count(*) FILTER (WHERE is_latest_revision) AS latest_rows
                    FROM security_listing_metrics
                    GROUP BY 1, 2, 3
                    HAVING count(*) FILTER (WHERE is_latest_revision) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("security_listing_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="security_listing_metrics",
            table_name="security_listing_metrics",
            check_name="bad_security_listing_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_listing_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR symbol IS NULL OR symbol = ''
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR coalesce(round_lot_size, 0) < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("security_listing_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="security_listing_metrics",
            table_name="security_listing_metrics",
            check_name="security_listing_metric_flags_consistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_listing_metrics
                WHERE (is_listing_compliant AND financial_status_code IS DISTINCT FROM 'N')
                   OR (is_noncompliant AND (NOT has_financial_status OR financial_status_code = 'N'))
                   OR (NOT has_financial_status AND (is_deficient OR is_delinquent OR is_bankrupt OR is_noncompliant OR is_listing_compliant))
                   OR (has_financial_status AND financial_status_code IS NULL)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("security_listing_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="form144_intent",
            table_name="form144_intent",
            check_name="duplicate_form144_intent_accessions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT accession_number, count(*) AS row_count
                    FROM form144_intent
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_intent",),
        ),
        SqlQualityCheck(
            dataset_id="form144_intent",
            table_name="form144_intent",
            check_name="form144_intent_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        coalesce(seller_cik, seller_name_norm, ''),
                        coalesce(security_id, ''),
                        coalesce(approx_sale_date, notice_date, filing_date),
                        coalesce(security_title, ''),
                        count(*) FILTER (WHERE coalesce(is_latest, false)) AS latest_rows
                    FROM form144_intent
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) FILTER (WHERE coalesce(is_latest, false)) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_intent",),
        ),
        SqlQualityCheck(
            dataset_id="form144_intent",
            table_name="form144_intent",
            check_name="bad_form144_intent_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM form144_intent
                WHERE filing_id IS NULL OR filing_id = ''
                   OR accession_number IS NULL OR accession_number = ''
                   OR seller_name IS NULL OR seller_name = ''
                   OR seller_name_norm IS NULL OR seller_name_norm = ''
                   OR source IS NULL OR source = ''
                   OR source_url IS NULL OR source_url = ''
                   OR available_at IS NULL
                   OR coalesce(as_of_date, notice_date, filing_date, approx_sale_date) IS NULL
                   OR (security_id IS NULL AND issuer_cik IS NULL AND issuer_trading_symbol IS NULL)
                   OR coalesce(shares_proposed, 0) < 0
                   OR coalesce(aggregate_market_value, 0) < 0
                   OR coalesce(approx_price_per_share, 0) < 0
                   OR coalesce(restatement_seq, 0) < 0
                   OR (sale_window_end_date IS NOT NULL
                       AND approx_sale_date IS NOT NULL
                       AND sale_window_end_date < approx_sale_date)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_intent",),
        ),
        SqlQualityCheck(
            dataset_id="form144_to_form4_link",
            table_name="form144_to_form4_link",
            check_name="bad_form144_to_form4_links",
            sql="""
                SELECT count(*)::DOUBLE
                FROM form144_to_form4_link
                WHERE form144_filing_id IS NULL OR form144_filing_id = ''
                   OR insider_transaction_id IS NULL OR insider_transaction_id = ''
                   OR source IS NULL OR source = ''
                   OR match_confidence IS NULL
                   OR match_confidence < 0
                   OR match_confidence > 1
                   OR match_method IS NULL
                   OR match_method = ''
                   OR match_status NOT IN ('FULL', 'PARTIAL', 'EXCESS', 'UNKNOWN')
                   OR available_at IS NULL
                   OR as_of_date IS NULL
                   OR coalesce(days_between, 0) < 0
                   OR coalesce(shares_proposed, 0) < 0
                   OR coalesce(transaction_shares, 0) < 0
                   OR coalesce(execution_ratio, 0) < 0
                   OR coalesce(shares_matched, 0) < 0
                   OR coalesce(value_matched, 0) < 0
                   OR coalesce(share_match_ratio, 0) < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_to_form4_link",),
        ),
        SqlQualityCheck(
            dataset_id="form144_to_form4_link",
            table_name="form144_to_form4_link",
            check_name="orphan_form144_to_form4_links",
            sql="""
                SELECT count(*)::DOUBLE
                FROM form144_to_form4_link l
                LEFT JOIN form144_intent f ON f.filing_id = l.form144_filing_id
                LEFT JOIN insider_transaction t ON t.transaction_id = l.insider_transaction_id
                WHERE f.filing_id IS NULL
                   OR t.transaction_id IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_to_form4_link", "form144_intent", "insider_transaction"),
        ),
        SqlQualityCheck(
            dataset_id="sec_blockholder_ownership",
            table_name="blockholder_filing",
            check_name="duplicate_blockholder_filings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT accession_number, count(*) AS row_count
                    FROM blockholder_filing
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("blockholder_filing",),
        ),
        SqlQualityCheck(
            dataset_id="sec_blockholder_ownership",
            table_name="blockholder_filing",
            check_name="bad_blockholder_filing_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM blockholder_filing
                WHERE filing_id IS NULL
                   OR filing_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR schedule_type NOT IN ('13D', '13D/A', '13G', '13G/A')
                   OR source_url IS NULL
                   OR source_url = ''
                   OR available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("blockholder_filing",),
        ),
        SqlQualityCheck(
            dataset_id="sec_blockholder_ownership",
            table_name="blockholder_reporting_person",
            check_name="bad_blockholder_reporting_person_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM blockholder_reporting_person
                WHERE reporting_person_id IS NULL
                   OR reporting_person_id = ''
                   OR filing_id IS NULL
                   OR filing_id = ''
                   OR reporting_person_seq IS NULL
                   OR reporting_person_seq < 1
                   OR reporting_person_name IS NULL
                   OR reporting_person_name = ''
                   OR coalesce(percent_of_class, 0) < 0
                   OR coalesce(percent_of_class, 0) > 100
            """,
            threshold=0.0,
            required_tables=("blockholder_reporting_person",),
        ),
        SqlQualityCheck(
            dataset_id="sec_blockholder_ownership",
            table_name="blockholder_reporting_person",
            check_name="orphan_blockholder_reporting_persons",
            sql="""
                SELECT count(*)::DOUBLE
                FROM blockholder_reporting_person p
                LEFT JOIN blockholder_filing f ON f.filing_id = p.filing_id
                WHERE f.filing_id IS NULL
            """,
            threshold=0.0,
            required_tables=("blockholder_reporting_person", "blockholder_filing"),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_candidates",
            table_name="identifier_resolution_candidates",
            check_name="duplicate_identifier_resolution_candidates",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source_dataset_id,
                        source_key_type,
                        source_key_value,
                        target_security_id,
                        coalesce(source_period, '') AS source_period,
                        match_method,
                        count(*) AS row_count
                    FROM identifier_resolution_candidates
                    GROUP BY 1, 2, 3, 4, 5, 6
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_candidates",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_candidates",
            table_name="identifier_resolution_candidates",
            check_name="bad_identifier_resolution_candidates",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_candidates
                WHERE source_dataset_id IS NULL
                   OR source_dataset_id = ''
                   OR source_key_type IS NULL
                   OR source_key_type = ''
                   OR source_key_value IS NULL
                   OR source_key_value = ''
                   OR target_security_id IS NULL
                   OR target_security_id = ''
                   OR confidence IS NULL
                   OR confidence < 0
                   OR confidence > 1
                   OR candidate_status NOT IN ('already_mapped', 'proposed', 'conflict')
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_candidates",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_candidates",
            table_name="identifier_resolution_candidates",
            check_name="orphan_identifier_resolution_targets",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_candidates c
                LEFT JOIN securities s
                  ON s.security_id = c.target_security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_candidates", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_candidates",
            table_name="identifier_resolution_candidates",
            check_name="missing_identifier_resolution_available_at",
            sql="SELECT count(*)::DOUBLE FROM identifier_resolution_candidates WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("identifier_resolution_candidates",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="identifier_resolution_decisions",
            check_name="duplicate_identifier_resolution_decisions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT candidate_id, decision_method, count(*) AS row_count
                    FROM identifier_resolution_decisions
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="identifier_resolution_decisions",
            check_name="bad_identifier_resolution_decisions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_decisions
                WHERE candidate_id IS NULL
                   OR candidate_id = ''
                   OR source_dataset_id IS NULL
                   OR source_dataset_id = ''
                   OR source_key_type IS NULL
                   OR source_key_type = ''
                   OR source_key_value IS NULL
                   OR source_key_value = ''
                   OR target_security_id IS NULL
                   OR target_security_id = ''
                   OR confidence IS NULL
                   OR confidence < 0
                   OR confidence > 1
                   OR candidate_status NOT IN ('already_mapped', 'proposed', 'conflict')
                   OR decision_status NOT IN ('accepted', 'rejected', 'needs_review')
                   OR decision_method IS NULL
                   OR decision_method = ''
                   OR decided_by IS NULL
                   OR decided_by = ''
                   OR decided_at IS NULL
                   OR effective_from IS NULL
                   OR as_of_date IS NULL
                   OR (decision_status = 'accepted' AND available_at IS NULL)
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="identifier_resolution_decisions",
            check_name="orphan_identifier_resolution_decision_candidates",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_decisions d
                LEFT JOIN identifier_resolution_candidates c
                  ON c.candidate_id = d.candidate_id
                WHERE c.candidate_id IS NULL
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions", "identifier_resolution_candidates"),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="identifier_resolution_decisions",
            check_name="orphan_identifier_resolution_decision_targets",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_decisions d
                LEFT JOIN securities s
                  ON s.security_id = d.target_security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="security_identifier_history",
            check_name="accepted_identifier_decisions_without_pit_identifier",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_decisions d
                LEFT JOIN security_identifier_history i
                  ON i.security_id = d.target_security_id
                 AND i.id_type = d.source_key_type
                 AND i.id_value = d.source_key_value
                 AND i.valid_from = d.effective_from
                 AND i.source = 'atx-db identifier decision manager'
                WHERE d.decision_status = 'accepted'
                  AND d.source_key_type = 'CUSIP'
                  AND i.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions", "security_identifier_history"),
        ),
    )
