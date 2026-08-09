from __future__ import annotations

from ._check_common import DEFAULT_EXPORT_OBJECTS, _export_scan_internal_cusip_sql
from ._types import SqlQualityCheck


def analytic_check_specs(
    *,
    daily_macro_stale_days: int,
    monthly_macro_stale_days: int,
    valuation_stale_gap_days: int,
) -> tuple[SqlQualityCheck, ...]:
    return (
        SqlQualityCheck(
            dataset_id="offexchange_quality_report",
            table_name="offexchange_quality_report",
            check_name="offexchange_quality_report_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, surface, input_source, period_type, period_start_date,
                           count(*) FILTER (WHERE is_latest_revision) AS latest_rows
                    FROM offexchange_quality_report
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) FILTER (WHERE is_latest_revision) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_quality_report",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_quality_report",
            table_name="offexchange_quality_report",
            check_name="offexchange_quality_report_bad_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_quality_report
                WHERE source IS NULL OR source = ''
                   OR surface NOT IN ('offexchange_volume', 'finra_short_volume')
                   OR period_type IS NULL OR period_type = ''
                   OR period_start_date IS NULL
                   OR available_at IS NULL
                   OR as_of_date IS NULL
                   OR period_end_date < period_start_date
                   OR coalesce(row_count, -1) < 0
                   OR coalesce(security_count, -1) < 0
                   OR coalesce(venue_or_market_count, -1) < 0
                   OR coalesce(restated_key_count, -1) < 0
                   OR coalesce(multiple_latest_key_count, -1) < 0
                   OR coalesce(bad_row_count, -1) < 0
                   OR coalesce(missing_available_at_count, -1) < 0
                   OR coalesce(high_short_flow_count, 0) < 0
                   OR coalesce(high_short_flow_count, 0) > coalesce(security_count, 0)
                   OR (short_volume_ratio IS NOT NULL AND (short_volume_ratio < 0 OR short_volume_ratio > 1))
                   OR (ats_share_pct IS NOT NULL AND (ats_share_pct < 0 OR ats_share_pct > 100))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_quality_report",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_quality_report",
            table_name="offexchange_quality_report",
            check_name="offexchange_quality_report_volume_consistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_quality_report
                WHERE (surface = 'offexchange_volume'
                       AND abs(coalesce(total_volume, 0)
                               - (coalesce(ats_volume, 0) + coalesce(non_ats_volume, 0))) > 1.0)
                   OR (surface = 'finra_short_volume'
                       AND (coalesce(short_volume, 0) > coalesce(total_volume, 0)
                            OR coalesce(short_exempt_volume, 0) > coalesce(total_volume, 0)))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_quality_report",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ratios",
            table_name="fundamental_ratios",
            check_name="duplicate_fundamental_ratio_natural_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, security_id, ratio_code, basis, period_end, count(*) AS n
                    FROM fundamental_ratios
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ratios",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ratios",
            table_name="fundamental_ratios",
            check_name="bad_fundamental_ratio_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ratios
                WHERE ratio_id IS NULL OR ratio_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR ratio_code IS NULL OR ratio_code = ''
                   OR ratio_category NOT IN ('profitability', 'leverage', 'cash_flow', 'payout', 'per_share', 'efficiency', 'growth', 'liquidity', 'health')
                   OR ratio_kind NOT IN ('ratio', 'level', 'per_share', 'growth', 'difference', 'score')
                   OR unit NOT IN ('ratio', 'currency', 'currency_per_share', 'score')
                   OR period_end IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR value IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ratios",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ratios",
            table_name="fundamental_ratios",
            check_name="non_finite_fundamental_ratio_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ratios
                WHERE value IS NOT NULL AND (isnan(value) OR isinf(value))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ratios",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_xbrl_metric",
            table_name="fundamental_xbrl_metric",
            check_name="duplicate_fundamental_xbrl_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT metric_id, count(*) AS n
                    FROM fundamental_xbrl_metric
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_xbrl_metric",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_xbrl_metric",
            table_name="fundamental_xbrl_metric",
            check_name="multiple_latest_fundamental_xbrl_metric_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, canonical_metric, period_type,
                           coalesce(CAST(period_start AS VARCHAR), ''), period_end, count(*) AS n
                    FROM fundamental_xbrl_metric
                    WHERE is_latest_revision
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_xbrl_metric",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_xbrl_metric",
            table_name="fundamental_xbrl_metric",
            check_name="bad_fundamental_xbrl_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_xbrl_metric
                WHERE metric_id IS NULL OR metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR canonical_metric IS NULL OR canonical_metric = ''
                   OR concept IS NULL OR concept = ''
                   OR period_type NOT IN ('instant', 'duration')
                   OR period_end IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_xbrl_metric",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_xbrl_metric",
            table_name="fundamental_xbrl_metric_ratio_universe_gap",
            check_name="xbrl_metric_covers_fundamental_ratio_universe",
            sql="""
                SELECT ratio_minus_xbrl_security_count::DOUBLE
                FROM fundamental_xbrl_metric_ratio_universe_gap
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_xbrl_metric_ratio_universe_gap",),
            detail_sql="""
                SELECT *
                FROM fundamental_xbrl_metric_ratio_universe_gap
                WHERE ratio_minus_xbrl_security_count > 0
            """,
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ratios",
            table_name="fundamental_ratios",
            check_name="ratio_kind_division_consistency",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ratios
                WHERE ratio_kind IN ('ratio', 'per_share')
                  AND denominator_value IS NOT NULL
                  AND numerator_value IS NOT NULL
                  AND denominator_value <> 0
                  AND abs(value - (numerator_value / denominator_value)) > 1e-6 * (1 + abs(value))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ratios",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="duplicate_valuation_multiple_natural_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, market_cap_source, security_id, trade_date, formula_code, count(*) AS n
                    FROM valuation_multiples
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="bad_valuation_multiple_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE valuation_multiple_id IS NULL OR valuation_multiple_id = ''
                   OR source IS NULL OR source = ''
                   OR market_cap_source IS NULL OR market_cap_source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR trade_date IS NULL
                   OR formula_code IS NULL OR formula_code = ''
                   OR category <> 'valuation'
                   OR kind NOT IN ('ratio', 'difference')
                   OR unit NOT IN ('ratio', 'currency')
                   OR period_end IS NULL
                   OR period_end > trade_date
                   OR value IS NULL
                   OR numerator_value IS NULL
                   OR denominator_value IS NULL
                   OR price IS NULL
                   OR market_cap IS NULL
                   OR is_meaningful IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR market_cap_available_at IS NULL
                   OR price_available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="non_finite_valuation_multiple_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE (value IS NOT NULL AND (isnan(value) OR isinf(value)))
                   OR (numerator_value IS NOT NULL AND (isnan(numerator_value) OR isinf(numerator_value)))
                   OR (denominator_value IS NOT NULL AND (isnan(denominator_value) OR isinf(denominator_value)))
                   OR (price IS NOT NULL AND (isnan(price) OR isinf(price)))
                   OR (market_cap IS NOT NULL AND (isnan(market_cap) OR isinf(market_cap)))
                   OR (enterprise_value IS NOT NULL AND (isnan(enterprise_value) OR isinf(enterprise_value)))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="valuation_multiple_arithmetic_consistency",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE (
                        kind = 'ratio'
                    AND denominator_value IS NOT NULL
                    AND numerator_value IS NOT NULL
                    AND denominator_value <> 0
                    AND abs(value - (numerator_value / denominator_value)) > 1e-6 * (1 + abs(value))
                )
                   OR (
                        formula_code = 'enterprise_value'
                    AND abs(value - (numerator_value - denominator_value)) > 1e-6 * (1 + abs(value))
                )
                   OR (
                        formula_code = 'enterprise_value'
                    AND enterprise_value IS NOT NULL
                    AND abs(enterprise_value - value) > 1e-6 * (1 + abs(enterprise_value))
                )
                   OR (
                        formula_code IN ('ev_to_ebitda', 'ev_to_sales')
                    AND enterprise_value IS NOT NULL
                    AND numerator_value IS NOT NULL
                    AND abs(enterprise_value - numerator_value) > 1e-6 * (1 + abs(enterprise_value))
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="valuation_multiple_non_positive_denominator_meaningfulness",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE coalesce(is_meaningful, false)
                  AND (
                        (kind = 'ratio' AND denominator_value <= 0)
                     OR market_cap <= 0
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="stale_price_fundamental_gap_days",
            sql=f"""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE trade_date IS NOT NULL
                  AND period_end IS NOT NULL
                  AND date_diff('day', period_end, trade_date) > {valuation_stale_gap_days}
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
            failure_status="warning",
            detail_sql=f"""
                SELECT
                    security_id,
                    symbol,
                    trade_date,
                    period_end,
                    formula_code,
                    date_diff('day', period_end, trade_date) AS gap_days,
                    available_at
                FROM valuation_multiples
                WHERE trade_date IS NOT NULL
                  AND period_end IS NOT NULL
                  AND date_diff('day', period_end, trade_date) > {valuation_stale_gap_days}
                ORDER BY gap_days DESC, security_id, formula_code
                LIMIT 20
            """,
        ),
        SqlQualityCheck(
            dataset_id="valuation_overlap_slice",
            table_name="valuation_overlap_slice",
            check_name="bad_valuation_overlap_slice_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_overlap_slice
                WHERE overlap_slice_id IS NULL OR overlap_slice_id = ''
                   OR source IS NULL OR source = ''
                   OR market_cap_sources_json IS NULL
                   OR symbol_scope_json IS NULL
                   OR numerator_security_count < 0
                   OR denominator_security_count < 0
                   OR valuation_row_count < 0
                   OR (coverage_ratio IS NOT NULL AND (coverage_ratio < 0 OR coverage_ratio > 1))
                   OR stale_price_fundamental_gap_days < 0
                   OR stale_valuation_row_count < 0
                   OR denominator_definition IS NULL OR denominator_definition = ''
                   OR details_json IS NULL OR details_json = ''
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_overlap_slice",),
        ),
        SqlQualityCheck(
            dataset_id="fact_disagreement",
            table_name="fact_disagreement",
            check_name="fact_disagreement_agreement_ratio",
            sql="""
                SELECT coalesce(
                    count(*) FILTER (WHERE agreement_status = 'agrees')::DOUBLE
                    / nullif(count(*)::DOUBLE, 0.0),
                    1.0
                )
                FROM fact_disagreement
            """,
            threshold=0.99,
            comparator="ge",
            required_tables=("fact_disagreement",),
            detail_sql="""
                SELECT
                    vendor,
                    security_id,
                    canonical_code,
                    basis,
                    period_end,
                    warehouse_value,
                    vendor_value,
                    absolute_difference,
                    relative_difference,
                    agreement_status
                FROM fact_disagreement
                WHERE agreement_status <> 'agrees'
                ORDER BY agreement_status, vendor, security_id, canonical_code, period_end
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fact_disagreement",
            table_name="fact_disagreement",
            check_name="bad_fact_disagreement_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fact_disagreement
                WHERE disagreement_id IS NULL OR disagreement_id = ''
                   OR source IS NULL OR source = ''
                   OR baseline_source IS NULL OR baseline_source = ''
                   OR vendor IS NULL OR vendor = ''
                   OR security_id IS NULL OR security_id = ''
                   OR item_id IS NULL
                   OR canonical_code IS NULL OR canonical_code = ''
                   OR basis IS NULL OR basis = ''
                   OR period_end IS NULL
                   OR vendor_value IS NULL
                   OR agreement_status NOT IN ('agrees', 'disagrees', 'missing_warehouse')
                   OR vintage_status IS NULL OR vintage_status = ''
                   OR is_latest_revision IS NULL
                   OR tolerance_abs < 0
                   OR tolerance_rel < 0
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR input_lineage_json IS NULL OR input_lineage_json = ''
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fact_disagreement",),
        ),
        SqlQualityCheck(
            dataset_id="short_interest_metrics",
            table_name="short_interest_metrics",
            check_name="duplicate_short_interest_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, security_id, settlement_date
                    FROM short_interest_metrics
                    GROUP BY 1, 2, 3 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_interest_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_interest_metrics",
            table_name="short_interest_metrics",
            check_name="bad_short_interest_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_interest_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR settlement_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR (days_to_cover_percentile IS NOT NULL
                       AND (days_to_cover_percentile < 0 OR days_to_cover_percentile > 1))
                   OR (short_interest_change_pct_percentile IS NOT NULL
                       AND (short_interest_change_pct_percentile < 0 OR short_interest_change_pct_percentile > 1))
                   OR (short_pressure_score IS NOT NULL
                       AND (short_pressure_score < 0 OR short_pressure_score > 100))
                   OR (average_daily_volume_percentile IS NOT NULL
                       AND (average_daily_volume_percentile < 0 OR average_daily_volume_percentile > 1))
                   OR (liquid_short_pressure_score IS NOT NULL
                       AND (liquid_short_pressure_score < 0 OR liquid_short_pressure_score > 100))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_interest_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_interest_metrics",
            table_name="short_interest_metrics",
            check_name="persistent_short_pressure_without_squeeze",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_interest_metrics
                WHERE coalesce(is_persistent_short_pressure, false)
                  AND NOT coalesce(is_squeeze_candidate, false)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_interest_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_interest_metrics",
            table_name="short_interest_metrics",
            check_name="liquid_short_pressure_without_tradeability",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_interest_metrics
                WHERE coalesce(is_liquid_short_pressure, false)
                  AND (
                      NOT coalesce(is_squeeze_candidate, false)
                      OR coalesce(average_daily_volume, 0) < 50000
                      OR coalesce(current_short_position, 0) < 100000
                      OR coalesce(short_pressure_score, 0) < 70
                      OR liquid_short_pressure_score IS NULL
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_interest_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="macro_metrics",
            table_name="macro_metrics",
            check_name="duplicate_macro_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, series_id, observation_date
                    FROM macro_metrics
                    GROUP BY 1, 2, 3 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("macro_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="macro_metrics",
            table_name="macro_metrics",
            check_name="bad_macro_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM macro_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR series_id IS NULL OR series_id = ''
                   OR observation_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("macro_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="macro_metrics",
            table_name="macro_metrics",
            check_name="missing_real_fedfunds_when_inputs_available",
            sql="""
                SELECT count(*)::DOUBLE
                FROM macro_metrics fed
                JOIN macro_metrics cpi
                  ON cpi.source = fed.source
                 AND cpi.series_id = 'CPIAUCSL'
                 AND cpi.observation_date = fed.observation_date
                 AND cpi.yoy_growth IS NOT NULL
                LEFT JOIN macro_metrics real
                  ON real.source = fed.source
                 AND real.series_id = 'REAL_FEDFUNDS'
                 AND real.observation_date = fed.observation_date
                WHERE fed.series_id = 'FEDFUNDS'
                  AND real.metric_id IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("macro_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="macro_metrics",
            table_name="macro_metrics",
            check_name="missing_sahm_rule_when_unrate_available",
            sql="""
                SELECT count(*)::DOUBLE
                FROM macro_metrics u
                LEFT JOIN macro_metrics sahm
                  ON sahm.source = u.source
                 AND sahm.series_id = 'SAHM_RULE'
                 AND sahm.observation_date = u.observation_date
                WHERE u.series_id = 'UNRATE'
                  AND sahm.metric_id IS NULL
                  AND (
                      SELECT count(*) FROM macro_metrics u2
                      WHERE u2.source = u.source
                        AND u2.series_id = 'UNRATE'
                        AND u2.observation_date <= u.observation_date
                  ) >= 14
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("macro_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="equity_price_metrics",
            table_name="equity_price_metrics",
            check_name="duplicate_equity_price_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, security_id, trade_date
                    FROM equity_price_metrics
                    GROUP BY 1, 2, 3 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("equity_price_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="equity_price_metrics",
            table_name="equity_price_metrics",
            check_name="bad_equity_price_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM equity_price_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR trade_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR (pct_from_high_252d IS NOT NULL AND pct_from_high_252d > 1e-9)
                   OR (realized_vol_20d IS NOT NULL AND realized_vol_20d < 0)
                   OR (avg_dollar_volume_21d IS NOT NULL AND avg_dollar_volume_21d < 0)
                   OR (amihud_illiquidity_21d IS NOT NULL AND amihud_illiquidity_21d < 0)
                   OR (max_drawdown_126d IS NOT NULL AND max_drawdown_126d > 1e-9)
                   OR (downside_deviation_60d IS NOT NULL AND downside_deviation_60d < 0)
                   OR (market_return_ew IS NOT NULL AND NOT isfinite(market_return_ew))
                   OR (beta_60d IS NOT NULL AND NOT isfinite(beta_60d))
                   OR (market_correlation_60d IS NOT NULL AND NOT isfinite(market_correlation_60d))
                   OR (market_correlation_60d IS NOT NULL AND (market_correlation_60d < -1.0000001 OR market_correlation_60d > 1.0000001))
                   OR (idiosyncratic_vol_60d IS NOT NULL AND NOT isfinite(idiosyncratic_vol_60d))
                   OR (idiosyncratic_vol_60d IS NOT NULL AND idiosyncratic_vol_60d < 0)
                   OR (daily_return_cs_pct_rank IS NOT NULL AND (NOT isfinite(daily_return_cs_pct_rank) OR daily_return_cs_pct_rank < 0 OR daily_return_cs_pct_rank > 1.0000001))
                   OR (momentum_21d_cs_pct_rank IS NOT NULL AND (NOT isfinite(momentum_21d_cs_pct_rank) OR momentum_21d_cs_pct_rank < 0 OR momentum_21d_cs_pct_rank > 1.0000001))
                   OR (realized_vol_20d_cs_pct_rank IS NOT NULL AND (NOT isfinite(realized_vol_20d_cs_pct_rank) OR realized_vol_20d_cs_pct_rank < 0 OR realized_vol_20d_cs_pct_rank > 1.0000001))
                   OR (dollar_volume_cs_pct_rank IS NOT NULL AND (NOT isfinite(dollar_volume_cs_pct_rank) OR dollar_volume_cs_pct_rank < 0 OR dollar_volume_cs_pct_rank > 1.0000001))
                   OR (amihud_illiquidity_21d_cs_pct_rank IS NOT NULL AND (NOT isfinite(amihud_illiquidity_21d_cs_pct_rank) OR amihud_illiquidity_21d_cs_pct_rank < 0 OR amihud_illiquidity_21d_cs_pct_rank > 1.0000001))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("equity_price_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_position_metrics",
            table_name="thirteenf_position_metrics",
            check_name="duplicate_thirteenf_position_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, manager_id, security_id, report_period
                    FROM thirteenf_position_metrics
                    GROUP BY 1, 2, 3, 4 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_position_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_position_metrics",
            table_name="thirteenf_position_metrics",
            check_name="bad_thirteenf_position_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_position_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR manager_id IS NULL OR manager_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR report_period IS NULL
                   OR as_of_date IS NULL
                   OR position_action NOT IN ('NEW', 'ADDED', 'TRIMMED', 'UNCHANGED', 'EXITED')
                   OR (is_closed_position AND shares_held <> 0)
                   OR (is_new_position AND shares_held_prev IS NOT NULL)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_position_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_option_metrics",
            table_name="thirteenf_option_metrics",
            check_name="duplicate_thirteenf_option_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT
                        source,
                        coalesce(security_id, ''),
                        coalesce(cusip, ''),
                        report_period,
                        coalesce(source_period, '')
                    FROM thirteenf_option_metrics
                    GROUP BY 1, 2, 3, 4, 5 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_option_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_option_metrics",
            table_name="thirteenf_option_metrics",
            check_name="bad_thirteenf_option_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_option_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR source IS NULL OR source = ''
                   OR (coalesce(security_id, '') = '' AND coalesce(cusip, '') = '')
                   OR report_period IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR option_bias NOT IN ('CALL_HEAVY', 'PUT_HEAVY', 'BALANCED', 'NO_OPTIONS')
                   OR option_manager_count < 0
                   OR call_manager_count < 0
                   OR put_manager_count < 0
                   OR option_position_count < 1
                   OR call_position_count < 0
                   OR put_position_count < 0
                   OR option_position_count <> call_position_count + put_position_count
                   OR call_share_quantity < 0
                   OR put_share_quantity < 0
                   OR call_value_usd < 0
                   OR put_value_usd < 0
                   OR common_share_quantity < 0
                   OR common_value_usd < 0
                   OR put_call_share_ratio < 0
                   OR put_call_value_ratio < 0
                   OR call_to_common_share_pct < 0
                   OR put_to_common_share_pct < 0
                   OR option_to_common_value_pct < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_option_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_option_metrics",
            table_name="thirteenf_option_metrics",
            check_name="mismatched_thirteenf_option_bias",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_option_metrics
                WHERE (
                        coalesce(call_share_quantity, 0) > coalesce(put_share_quantity, 0)
                        AND option_bias <> 'CALL_HEAVY'
                    )
                   OR (
                        coalesce(put_share_quantity, 0) > coalesce(call_share_quantity, 0)
                        AND option_bias <> 'PUT_HEAVY'
                    )
                   OR (
                        coalesce(call_share_quantity, 0) = coalesce(put_share_quantity, 0)
                        AND coalesce(call_share_quantity, 0) > 0
                        AND option_bias <> 'BALANCED'
                    )
                   OR (
                        coalesce(call_share_quantity, 0) = 0
                        AND coalesce(put_share_quantity, 0) = 0
                        AND option_bias <> 'NO_OPTIONS'
                    )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_option_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_concentration_metrics",
            table_name="thirteenf_concentration_metrics",
            check_name="duplicate_thirteenf_concentration_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT
                        source,
                        coalesce(security_id, ''),
                        coalesce(cusip, ''),
                        report_period,
                        coalesce(source_period, '')
                    FROM thirteenf_concentration_metrics
                    GROUP BY 1, 2, 3, 4, 5 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_concentration_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_concentration_metrics",
            table_name="thirteenf_concentration_metrics",
            check_name="bad_thirteenf_concentration_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_concentration_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR source IS NULL OR source = ''
                   OR (coalesce(security_id, '') = '' AND coalesce(cusip, '') = '')
                   OR report_period IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR filing_count < 1
                   OR holder_count < 1
                   OR common_value_usd < 0
                   OR common_share_quantity < 0
                   OR top_holder_value_usd < 0
                   OR top_holder_share_quantity < 0
                   OR top_holder_value_pct < 0 OR top_holder_value_pct > 1.0000001
                   OR top_holder_share_pct < 0 OR top_holder_share_pct > 1.0000001
                   OR top_3_holder_value_pct < 0 OR top_3_holder_value_pct > 1.0000001
                   OR top_5_holder_value_pct < 0 OR top_5_holder_value_pct > 1.0000001
                   OR top_10_holder_value_pct < 0 OR top_10_holder_value_pct > 1.0000001
                   OR value_hhi < 0 OR value_hhi > 1.0000001
                   OR share_hhi < 0 OR share_hhi > 1.0000001
                   OR effective_holder_count_value < 1
                   OR effective_holder_count_share < 1
                   OR concentration_bucket NOT IN ('DISPERSED', 'MODERATE', 'CONCENTRATED', 'HIGHLY_CONCENTRATED')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_concentration_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_concentration_metrics",
            table_name="thirteenf_concentration_metrics",
            check_name="mismatched_thirteenf_concentration_ordering",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_concentration_metrics
                WHERE top_holder_value_pct > top_3_holder_value_pct + 1e-9
                   OR top_3_holder_value_pct > top_5_holder_value_pct + 1e-9
                   OR top_5_holder_value_pct > top_10_holder_value_pct + 1e-9
                   OR top_10_holder_value_pct > 1.0000001
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_concentration_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_dividend_metrics",
            table_name="corporate_action_dividend_metrics",
            check_name="duplicate_corporate_action_dividend_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, security_id, ex_date
                    FROM corporate_action_dividend_metrics
                    GROUP BY 1, 2, 3 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_dividend_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_dividend_metrics",
            table_name="corporate_action_dividend_metrics",
            check_name="bad_corporate_action_dividend_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_dividend_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR ex_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR (dividend_yield_spot IS NOT NULL AND dividend_yield_spot < 0)
                   OR (ttm_dividend_count IS NOT NULL AND ttm_dividend_count < 1)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_dividend_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_split_metrics",
            table_name="corporate_action_split_metrics",
            check_name="duplicate_corporate_action_split_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, factor_source, daily_adjustment_source, coalesce(bar_source, ''), event_ref_id
                    FROM corporate_action_split_metrics
                    GROUP BY 1, 2, 3, 4, 5 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_split_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_split_metrics",
            table_name="corporate_action_split_metrics",
            check_name="bad_corporate_action_split_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_split_metrics
                WHERE split_metric_id IS NULL OR split_metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR event_ref_id IS NULL OR event_ref_id = ''
                   OR ex_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR factor_price <= 0
                   OR factor_shares <= 0
                   OR reconciliation_status NOT IN ('RECONCILED', 'MISMATCH', 'MISSING_DAILY_FACTOR')
                   OR (is_reconciled AND reconciliation_status <> 'RECONCILED')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_split_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_split_metrics",
            table_name="corporate_action_split_metrics",
            check_name="mismatched_corporate_action_split_factors",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_split_metrics
                WHERE reconciliation_status = 'MISMATCH'
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_split_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_factor_reconciliation",
            table_name="corporate_action_factor_reconciliation",
            check_name="duplicate_corporate_action_factor_reconciliation_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT
                        source,
                        factor_source,
                        daily_adjustment_source,
                        coalesce(bar_source, ''),
                        event_ref_id,
                        event_type
                    FROM corporate_action_factor_reconciliation
                    GROUP BY 1, 2, 3, 4, 5, 6 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_factor_reconciliation",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_factor_reconciliation",
            table_name="corporate_action_factor_reconciliation",
            check_name="bad_corporate_action_factor_reconciliation_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_factor_reconciliation
                WHERE reconciliation_id IS NULL OR reconciliation_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR event_ref_id IS NULL OR event_ref_id = ''
                   OR event_type IS NULL OR event_type = ''
                   OR ex_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR factor_price <= 0
                   OR factor_shares <= 0
                   OR same_day_event_count < 1
                   OR reconciliation_status NOT IN (
                       'RECONCILED',
                       'MISMATCH',
                       'MISSING_DAILY_FACTOR',
                       'COMPOUND_EVENT',
                       'UNSUPPORTED_EVENT_TYPE'
                   )
                   OR (is_reconciled AND reconciliation_status <> 'RECONCILED')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_factor_reconciliation",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_factor_reconciliation",
            table_name="corporate_action_factor_reconciliation",
            check_name="mismatched_corporate_action_factor_steps",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_factor_reconciliation
                WHERE reconciliation_status = 'MISMATCH'
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_factor_reconciliation",),
        ),
    )
