from __future__ import annotations

from ._check_common import DEFAULT_EXPORT_OBJECTS, _export_scan_internal_cusip_sql
from ._types import SqlQualityCheck


def market_reference_check_specs(
    *,
    daily_macro_stale_days: int,
    monthly_macro_stale_days: int,
    valuation_stale_gap_days: int,
) -> tuple[SqlQualityCheck, ...]:
    return (
        SqlQualityCheck(
            dataset_id="fundamental_standardized",
            table_name="v_fundamental_standardization_coverage",
            check_name="fundamental_standardization_exception_rate",
            sql="""
                SELECT coalesce(max(
                    CASE
                        WHEN populated_item_count + exception_count = 0 THEN 0.0
                        ELSE exception_count / (populated_item_count + exception_count)
                    END
                ), 0.0)::DOUBLE
                FROM v_fundamental_standardization_coverage
            """,
            threshold=0.25,
            comparator="le",
            required_tables=(
                "fundamental_standardized",
                "fundamental_standardization_exception",
                "v_fundamental_standardization_coverage",
            ),
            detail_sql="""
                SELECT
                    source,
                    security_id,
                    period_end,
                    basis,
                    populated_item_count,
                    exception_count,
                    CASE
                        WHEN populated_item_count + exception_count = 0 THEN 0.0
                        ELSE exception_count / (populated_item_count + exception_count)
                    END AS exception_rate
                FROM v_fundamental_standardization_coverage
                WHERE populated_item_count + exception_count > 0
                  AND exception_count / (populated_item_count + exception_count) > 0.25
                ORDER BY exception_rate DESC, security_id, period_end
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_standardized",
            table_name="v_fundamental_standardization_coverage",
            check_name="fundamental_standardization_template_coverage",
            sql="""
                SELECT coalesce(min(coverage_ratio), 1.0)::DOUBLE
                FROM v_fundamental_standardization_coverage
                WHERE populated_item_count + exception_count > 0
            """,
            threshold=0.05,
            comparator="ge",
            required_tables=("fundamental_standardized", "v_fundamental_standardization_coverage"),
            detail_sql="""
                SELECT
                    source,
                    security_id,
                    period_end,
                    basis,
                    populated_item_count,
                    template_item_count,
                    coverage_ratio,
                    exception_count
                FROM v_fundamental_standardization_coverage
                WHERE populated_item_count + exception_count > 0
                  AND coverage_ratio < 0.05
                ORDER BY coverage_ratio, security_id, period_end
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="universe_membership",
            table_name="universe_membership",
            check_name="priced_fundamental_universe_decision_coverage",
            sql="""
                WITH price_days AS (
                    SELECT
                        security_id,
                        trade_date,
                        max(available_at) AS price_available_at
                    FROM equity_daily_bars
                    WHERE security_id IS NOT NULL
                      AND trade_date IS NOT NULL
                      AND close IS NOT NULL
                      AND close > 0
                    GROUP BY security_id, trade_date
                ),
                priced_fundamental_days AS (
                    SELECT p.*
                    FROM price_days p
                    WHERE EXISTS (
                        SELECT 1
                        FROM fundamental_points f
                        WHERE f.security_id = p.security_id
                          AND f.period_end IS NOT NULL
                          AND f.period_end <= p.trade_date
                          AND f.value IS NOT NULL
                          AND (
                              f.available_at IS NULL
                              OR p.price_available_at IS NULL
                              OR f.available_at <= p.price_available_at
                          )
                    )
                )
                SELECT count(*)::DOUBLE
                FROM priced_fundamental_days p
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM universe_membership u
                    WHERE u.security_id = p.security_id
                      AND u.valid_from <= p.trade_date
                      AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
                      AND u.as_of_date <= p.trade_date
                      AND u.is_latest_revision
                      AND (
                          u.available_at IS NULL
                          OR p.price_available_at IS NULL
                          OR u.available_at <= p.price_available_at
                      )
                )
            """,
            threshold=0.0,
            required_tables=("equity_daily_bars", "fundamental_points", "universe_membership"),
            detail_sql="""
                WITH price_days AS (
                    SELECT
                        security_id,
                        max(symbol) AS symbol,
                        trade_date,
                        max(available_at) AS price_available_at
                    FROM equity_daily_bars
                    WHERE security_id IS NOT NULL
                      AND trade_date IS NOT NULL
                      AND close IS NOT NULL
                      AND close > 0
                    GROUP BY security_id, trade_date
                ),
                priced_fundamental_days AS (
                    SELECT p.*
                    FROM price_days p
                    WHERE EXISTS (
                        SELECT 1
                        FROM fundamental_points f
                        WHERE f.security_id = p.security_id
                          AND f.period_end IS NOT NULL
                          AND f.period_end <= p.trade_date
                          AND f.value IS NOT NULL
                          AND (
                              f.available_at IS NULL
                              OR p.price_available_at IS NULL
                              OR f.available_at <= p.price_available_at
                          )
                    )
                )
                SELECT
                    p.security_id,
                    p.symbol,
                    p.trade_date,
                    p.price_available_at
                FROM priced_fundamental_days p
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM universe_membership u
                    WHERE u.security_id = p.security_id
                      AND u.valid_from <= p.trade_date
                      AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
                      AND u.as_of_date <= p.trade_date
                      AND u.is_latest_revision
                      AND (
                          u.available_at IS NULL
                          OR p.price_available_at IS NULL
                          OR u.available_at <= p.price_available_at
                      )
                )
                ORDER BY p.trade_date, p.security_id
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="valuation_input_coverage",
            table_name="v_valuation_input_coverage",
            check_name="valuation_input_core_completeness",
            sql="""
                WITH universe_days AS (
                    SELECT DISTINCT
                        security_id,
                        symbol,
                        valid_from AS as_of_date,
                        coalesce(available_at, CAST(valid_from AS TIMESTAMP)) AS decision_available_at
                    FROM universe_membership
                    WHERE is_member
                      AND is_latest_revision
                ),
                required_inputs AS (
                    SELECT canonical_input
                    FROM valuation_input_catalog
                    WHERE is_core_input
                ),
                missing_inputs AS (
                    SELECT
                        u.security_id,
                        u.as_of_date,
                        r.canonical_input
                    FROM universe_days u
                    CROSS JOIN required_inputs r
                    WHERE NOT EXISTS (
                        SELECT 1
                        FROM v_valuation_input_coverage c
                        WHERE c.security_id = u.security_id
                          AND c.canonical_input = r.canonical_input
                          AND c.input_date <= u.as_of_date
                          AND (
                              c.available_at IS NULL
                              OR c.available_at <= u.decision_available_at
                          )
                    )
                )
                SELECT count(*)::DOUBLE
                FROM missing_inputs
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=(
                "valuation_input_catalog",
                "v_valuation_input_coverage",
                "universe_membership",
            ),
            detail_sql="""
                WITH universe_days AS (
                    SELECT DISTINCT
                        security_id,
                        symbol,
                        valid_from AS as_of_date,
                        coalesce(available_at, CAST(valid_from AS TIMESTAMP)) AS decision_available_at
                    FROM universe_membership
                    WHERE is_member
                      AND is_latest_revision
                ),
                required_inputs AS (
                    SELECT canonical_input, display_name, source_table
                    FROM valuation_input_catalog
                    WHERE is_core_input
                )
                SELECT
                    u.security_id,
                    u.symbol,
                    u.as_of_date,
                    u.decision_available_at,
                    r.canonical_input,
                    r.display_name,
                    r.source_table
                FROM universe_days u
                CROSS JOIN required_inputs r
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM v_valuation_input_coverage c
                    WHERE c.security_id = u.security_id
                      AND c.canonical_input = r.canonical_input
                      AND c.input_date <= u.as_of_date
                      AND (
                          c.available_at IS NULL
                          OR c.available_at <= u.decision_available_at
                      )
                )
                ORDER BY u.as_of_date, u.security_id, r.canonical_input
                LIMIT 50
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="valuation_input_coverage",
            table_name="v_valuation_input_coverage",
            check_name="valuation_core_item_stub_detector",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_input_catalog c
                WHERE c.is_core_input
                  AND NOT EXISTS (
                      SELECT 1
                      FROM v_valuation_input_coverage v
                      WHERE v.canonical_input = c.canonical_input
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_input_catalog", "v_valuation_input_coverage"),
            detail_sql="""
                SELECT
                    c.canonical_input,
                    c.display_name,
                    c.source_table,
                    c.source_field,
                    c.source_filter
                FROM valuation_input_catalog c
                WHERE c.is_core_input
                  AND NOT EXISTS (
                      SELECT 1
                      FROM v_valuation_input_coverage v
                      WHERE v.canonical_input = c.canonical_input
                  )
                ORDER BY c.canonical_input
                LIMIT 50
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="tbltickerhistory_daily",
            table_name="equity_daily_bars",
            check_name="duplicate_equity_daily_bars",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, security_id, trade_date, count(*) AS row_count
                    FROM equity_daily_bars
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("equity_daily_bars",),
        ),
        SqlQualityCheck(
            dataset_id="tbltickerhistory_daily",
            table_name="equity_daily_bars",
            check_name="bad_ohlcv_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM equity_daily_bars
                WHERE volume < 0
                   OR open <= 0
                   OR high <= 0
                   OR low <= 0
                   OR close <= 0
                   OR high < greatest(open, low, close)
                   OR low > least(open, high, close)
            """,
            threshold=0.0,
            required_tables=("equity_daily_bars",),
        ),
        SqlQualityCheck(
            dataset_id="tbltickerhistory_daily",
            table_name="equity_daily_bars",
            check_name="orphan_equity_daily_bars",
            sql="""
                SELECT count(*)::DOUBLE
                FROM equity_daily_bars b
                LEFT JOIN securities s ON s.security_id = b.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("equity_daily_bars", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="tbltickerhistory_daily",
            table_name="equity_daily_bars",
            check_name="missing_bar_available_at",
            sql="SELECT count(*)::DOUBLE FROM equity_daily_bars WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("equity_daily_bars",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_actions",
            table_name="corporate_actions",
            check_name="duplicate_corporate_actions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        action_type,
                        ex_date,
                        coalesce(details_json, '') AS details_json,
                        count(*) AS row_count
                    FROM corporate_actions
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("corporate_actions",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_actions",
            table_name="corporate_actions",
            check_name="bad_corporate_action_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_actions
                WHERE ex_date IS NULL
                   OR security_id IS NULL
                   OR action_type IS NULL
                   OR (cash_amount IS NOT NULL AND cash_amount < 0)
                   OR (adjustment_factor IS NOT NULL AND adjustment_factor <= 0)
            """,
            threshold=0.0,
            required_tables=("corporate_actions",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_actions",
            table_name="corporate_actions",
            check_name="orphan_corporate_actions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_actions c
                LEFT JOIN securities s ON s.security_id = c.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("corporate_actions", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="adjustment_factor_history",
            table_name="corp_action_type_dim",
            check_name="bad_corp_action_type_dim_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corp_action_type_dim
                WHERE type_code IS NULL
                   OR event_type IS NULL
                   OR event_type = ''
                   OR category IS NULL
                   OR category = ''
                   OR sub_category IS NULL
                   OR sub_category = ''
                   OR description IS NULL
                   OR description = ''
                   OR affects_price IS NULL
                   OR affects_shares IS NULL
                   OR mandatory IS NULL
            """,
            threshold=0.0,
            required_tables=("corp_action_type_dim",),
        ),
        SqlQualityCheck(
            dataset_id="adjustment_factor_history",
            table_name="adjustment_factor_history",
            check_name="duplicate_adjustment_factor_history",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT adjustment_factor_id, count(*) AS row_count
                    FROM adjustment_factor_history
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("adjustment_factor_history",),
        ),
        SqlQualityCheck(
            dataset_id="adjustment_factor_history",
            table_name="adjustment_factor_history",
            check_name="bad_adjustment_factor_history_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM adjustment_factor_history a
                LEFT JOIN corp_action_type_dim d ON d.type_code = a.type_code
                WHERE a.adjustment_factor_id IS NULL
                   OR a.adjustment_factor_id = ''
                   OR a.source IS NULL
                   OR a.source = ''
                   OR a.source_action_source IS NULL
                   OR a.source_action_source = ''
                   OR a.security_id IS NULL
                   OR a.security_id = ''
                   OR a.ex_date IS NULL
                   OR a.event_type IS NULL
                   OR a.event_type = ''
                   OR a.classification_reason IS NULL
                   OR a.classification_reason = ''
                   OR d.type_code IS NULL
                   OR d.event_type <> a.event_type
                   OR a.event_ref_id IS NULL
                   OR a.event_ref_id = ''
                   OR a.factor_price <= 0
                   OR a.factor_shares <= 0
                   OR a.factor_volume <= 0
                   OR a.cumulative_price_factor <= 0
                   OR a.cumulative_share_factor <= 0
                   OR a.available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("adjustment_factor_history", "corp_action_type_dim"),
        ),
        SqlQualityCheck(
            dataset_id="adjustment_factor_history",
            table_name="adjustment_factor_history",
            check_name="orphan_adjustment_factor_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM adjustment_factor_history a
                LEFT JOIN securities s ON s.security_id = a.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("adjustment_factor_history", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="daily_adjustment_factors",
            table_name="daily_adjustment_factors",
            check_name="duplicate_daily_adjustment_factors",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        bar_source,
                        factor_source,
                        security_id,
                        trade_date,
                        as_of_date,
                        count(*) AS row_count
                    FROM daily_adjustment_factors
                    GROUP BY 1, 2, 3, 4, 5, 6
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("daily_adjustment_factors",),
        ),
        SqlQualityCheck(
            dataset_id="daily_adjustment_factors",
            table_name="daily_adjustment_factors",
            check_name="bad_daily_adjustment_factor_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM daily_adjustment_factors d
                WHERE d.daily_adjustment_id IS NULL
                   OR d.daily_adjustment_id = ''
                   OR d.source IS NULL
                   OR d.source = ''
                   OR d.bar_source IS NULL
                   OR d.bar_source = ''
                   OR d.factor_source IS NULL
                   OR d.factor_source = ''
                   OR d.security_id IS NULL
                   OR d.security_id = ''
                   OR d.trade_date IS NULL
                   OR d.as_of_date IS NULL
                   OR d.as_of_date < d.trade_date
                   OR d.split_price_factor <= 0
                   OR d.split_share_factor <= 0
                   OR d.dividend_total_return_factor <= 0
                   OR d.total_return_price_factor <= 0
                   OR d.raw_close <= 0
                   OR d.split_adjusted_close <= 0
                   OR d.total_return_adjusted_close <= 0
                   OR d.visible_event_count < 0
                   OR d.split_event_count < 0
                   OR d.cash_div_event_count < 0
                   OR d.available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("daily_adjustment_factors",),
        ),
        SqlQualityCheck(
            dataset_id="daily_adjustment_factors",
            table_name="daily_adjustment_factors",
            check_name="orphan_daily_adjustment_factor_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM daily_adjustment_factors d
                LEFT JOIN securities s ON s.security_id = d.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("daily_adjustment_factors", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="shares_outstanding_history",
            table_name="shares_outstanding_history",
            check_name="duplicate_shares_outstanding_history",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT share_history_id, count(*) AS row_count
                    FROM shares_outstanding_history
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("shares_outstanding_history",),
        ),
        SqlQualityCheck(
            dataset_id="shares_outstanding_history",
            table_name="shares_outstanding_history",
            check_name="bad_shares_outstanding_history_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM shares_outstanding_history
                WHERE share_history_id IS NULL
                   OR share_history_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR share_count_type NOT IN (
                        'shares_outstanding', 'shares_basic_avg', 'shares_diluted_avg',
                        'float', 'treasury', 'class_a', 'class_b', 'class_c', 'class_d'
                   )
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR period_type NOT IN ('instant', 'duration')
                   OR period_end IS NULL
                   OR effective_date IS NULL
                   OR effective_date <> period_end
                   OR as_of_date IS NULL
                   OR period_end > as_of_date
                   OR available_at IS NULL
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR share_count < 0
                   OR source_url IS NULL
                   OR source_url = ''
            """,
            threshold=0.0,
            required_tables=("shares_outstanding_history",),
        ),
        SqlQualityCheck(
            dataset_id="shares_outstanding_history",
            table_name="shares_outstanding_history",
            check_name="float_shares_not_above_outstanding",
            sql="""
                SELECT count(*)::DOUBLE
                FROM shares_outstanding_history f
                JOIN shares_outstanding_history s
                  ON s.source = f.source
                 AND s.security_id = f.security_id
                 AND s.accession_number = f.accession_number
                 AND s.period_end = f.period_end
                 AND s.as_of_date = f.as_of_date
                 AND s.share_count_type = 'shares_outstanding'
                WHERE f.share_count_type = 'float'
                  AND f.share_count > s.share_count
            """,
            threshold=0.0,
            required_tables=("shares_outstanding_history",),
        ),
        SqlQualityCheck(
            dataset_id="shares_outstanding_history",
            table_name="shares_outstanding_history",
            check_name="orphan_shares_outstanding_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM shares_outstanding_history h
                LEFT JOIN securities s ON s.security_id = h.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("shares_outstanding_history", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="duplicate_identifier_history_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, id_type, id_value, valid_from, count(*) AS row_count
                    FROM security_identifier_history
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("security_identifier_history",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="bad_identifier_validity_ranges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_identifier_history
                WHERE valid_to IS NOT NULL
                  AND valid_to <= valid_from
            """,
            threshold=0.0,
            required_tables=("security_identifier_history",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="orphan_identifier_history",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_identifier_history i
                LEFT JOIN securities s ON s.security_id = i.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("security_identifier_history", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="identifier_multi_security_overlaps",
            # Ticker/identifier uniqueness-per-window is a reference-master
            # invariant for security-level identifiers. ENTITY_ID is above
            # security_id and may be shared after M&A. The raw tbltickerhistory
            # price feed legitimately violates
            # it (share classes trade concurrently, tickers are recycled across
            # issuers, missing vendor ids), the same way CRSP keys on PERMNO not
            # ticker — so price-feed securities are excluded here. Intra-security
            # duplicate intervals are still caught by the self-overlap check below.
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_identifier_history a
                JOIN security_identifier_history b
                  ON a.id_type = b.id_type
                 AND a.id_value = b.id_value
                 AND a.security_id <> b.security_id
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
                 AND a.security_id < b.security_id
                WHERE a.id_type <> 'ENTITY_ID'
                  AND a.source NOT LIKE 'tbltickerhistory%'
                  AND b.source NOT LIKE 'tbltickerhistory%'
            """,
            threshold=0.0,
            required_tables=("security_identifier_history",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="identifier_same_source_self_overlaps",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_identifier_history a
                JOIN security_identifier_history b
                  ON a.security_id = b.security_id
                 AND a.id_type = b.id_type
                 AND a.id_value = b.id_value
                 AND a.source = b.source
                 AND a.valid_from < b.valid_from
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
            """,
            threshold=0.0,
            required_tables=("security_identifier_history",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="export_scan_internal_cusip_leak",
            # Enforces the 13f_holdings.md B.3 boundary: internal_cusip (see
            # migration 0079's field_catalog note) is internal-only matching
            # support and must never appear on any lake-exported / public /
            # catalogued-public object. Today the boundary is enforced by
            # OMISSION from lake.DEFAULT_EXPORT_OBJECTS; this check turns that
            # into an enforced invariant instead of a tribal-knowledge
            # omission -- it fails if internal_cusip is ever added as a
            # column on any exported object.
            sql=_export_scan_internal_cusip_sql(DEFAULT_EXPORT_OBJECTS),
            threshold=0.0,
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="exchange_listings",
            check_name="orphan_exchange_listings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM exchange_listings l
                LEFT JOIN securities s ON s.security_id = l.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("exchange_listings", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="exchange_listings",
            check_name="listing_multi_security_overlaps",
            # See identifier_multi_security_overlaps: ticker uniqueness is a
            # reference-master invariant, not a raw price-feed one, so
            # tbltickerhistory securities are excluded.
            sql="""
                SELECT count(*)::DOUBLE
                FROM exchange_listings a
                JOIN exchange_listings b
                  ON a.ticker = b.ticker
                 AND coalesce(a.exchange_code, '') = coalesce(b.exchange_code, '')
                 AND a.security_id <> b.security_id
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
                 AND a.security_id < b.security_id
                WHERE a.source NOT LIKE 'tbltickerhistory%'
                  AND b.source NOT LIKE 'tbltickerhistory%'
            """,
            threshold=0.0,
            required_tables=("exchange_listings",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="exchange_listings",
            check_name="listing_same_source_self_overlaps",
            sql="""
                SELECT count(*)::DOUBLE
                FROM exchange_listings a
                JOIN exchange_listings b
                  ON a.security_id = b.security_id
                 AND a.ticker = b.ticker
                 AND coalesce(a.exchange_code, '') = coalesce(b.exchange_code, '')
                 AND a.source = b.source
                 AND a.valid_from < b.valid_from
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
            """,
            threshold=0.0,
            required_tables=("exchange_listings",),
        ),
        SqlQualityCheck(
            dataset_id="nasdaq_listing_events",
            table_name="nasdaq_listing_events",
            check_name="duplicate_listing_event_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT event_id, count(*) AS row_count
                    FROM nasdaq_listing_events
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("nasdaq_listing_events",),
        ),
        SqlQualityCheck(
            dataset_id="nasdaq_listing_events",
            table_name="nasdaq_listing_events",
            check_name="bad_listing_event_required_fields",
            sql="""
                SELECT count(*)::DOUBLE
                FROM nasdaq_listing_events
                WHERE symbol IS NULL
                   OR symbol = ''
                   OR effective_date IS NULL
                   OR as_of_date IS NULL
                   OR source_url IS NULL
            """,
            threshold=0.0,
            required_tables=("nasdaq_listing_events",),
        ),
        SqlQualityCheck(
            dataset_id="nasdaq_listing_events",
            table_name="nasdaq_listing_events",
            check_name="bad_listing_event_actions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM nasdaq_listing_events
                WHERE coalesce(nasdaq_action, '') NOT IN ('', 'Add', 'Delete')
                   OR coalesce(bx_action, '') NOT IN ('', 'Add', 'Delete')
                   OR coalesce(psx_action, '') NOT IN ('', 'Add', 'Delete')
            """,
            threshold=0.0,
            required_tables=("nasdaq_listing_events",),
        ),
        SqlQualityCheck(
            dataset_id="nasdaq_listing_events",
            table_name="nasdaq_listing_events",
            check_name="listing_event_future_asof",
            sql="""
                SELECT count(*)::DOUBLE
                FROM nasdaq_listing_events
                WHERE as_of_date > current_date
            """,
            threshold=0.0,
            required_tables=("nasdaq_listing_events",),
        ),
        SqlQualityCheck(
            dataset_id="listing_status_intervals",
            table_name="listing_status_intervals",
            check_name="duplicate_listing_status_interval_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT listing_status_id, count(*) AS row_count
                    FROM listing_status_intervals
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("listing_status_intervals",),
        ),
        SqlQualityCheck(
            dataset_id="listing_status_intervals",
            table_name="listing_status_intervals",
            check_name="bad_listing_status_intervals",
            sql="""
                SELECT count(*)::DOUBLE
                FROM listing_status_intervals
                WHERE symbol IS NULL
                   OR symbol = ''
                   OR status NOT IN ('active', 'inactive')
                   OR valid_from IS NULL
                   OR (valid_to IS NOT NULL AND valid_to <= valid_from)
                   OR as_of_date IS NULL
                   OR source IS NULL
                   OR source = ''
                   OR evidence_source IS NULL
                   OR evidence_source = ''
                   OR method IS NULL
                   OR method = ''
            """,
            threshold=0.0,
            required_tables=("listing_status_intervals",),
        ),
        SqlQualityCheck(
            dataset_id="listing_status_intervals",
            table_name="listing_status_intervals",
            check_name="orphan_listing_status_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM listing_status_intervals l
                LEFT JOIN securities s
                  ON s.security_id = l.security_id
                WHERE l.security_id IS NOT NULL
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("listing_status_intervals", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="listing_status_intervals",
            table_name="listing_status_intervals",
            check_name="listing_status_same_method_overlaps",
            sql="""
                SELECT count(*)::DOUBLE
                FROM listing_status_intervals a
                JOIN listing_status_intervals b
                  ON a.source = b.source
                 AND a.evidence_source = b.evidence_source
                 AND a.method = b.method
                 AND a.symbol = b.symbol
                 AND coalesce(a.listing_venue_code, '') = coalesce(b.listing_venue_code, '')
                 AND a.status = b.status
                 AND a.listing_status_id < b.listing_status_id
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
            """,
            threshold=0.0,
            required_tables=("listing_status_intervals",),
        ),
        SqlQualityCheck(
            dataset_id="delist_code_dim",
            table_name="delist_code_dim",
            check_name="bad_delist_code_dim_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delist_code_dim
                WHERE delist_code IS NULL
                   OR delist_code = ''
                   OR code_system IS NULL
                   OR code_system = ''
                   OR reason_category IS NULL
                   OR reason_category = ''
                   OR description IS NULL
                   OR description = ''
                   OR imputation_policy IS NULL
                   OR imputation_policy = ''
                   OR source IS NULL
                   OR source = ''
                   OR (
                       default_imputed_return IS NOT NULL
                       AND (default_imputed_return < -1.0 OR default_imputed_return > 10.0)
                   )
            """,
            threshold=0.0,
            required_tables=("delist_code_dim",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="duplicate_delisting_events",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        listing_status_source,
                        source_listing_status_id,
                        delist_code,
                        count(*) AS row_count
                    FROM delisting_events
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("delisting_events",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="bad_delisting_event_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                WHERE d.delisting_event_id IS NULL
                   OR d.delisting_event_id = ''
                   OR d.source IS NULL
                   OR d.source = ''
                   OR d.listing_status_source IS NULL
                   OR d.listing_status_source = ''
                   OR d.source_listing_status_id IS NULL
                   OR d.source_listing_status_id = ''
                   OR d.symbol IS NULL
                   OR d.symbol = ''
                   OR d.delist_date IS NULL
                   OR d.as_of_date IS NULL
                   OR d.available_at IS NULL
                   OR d.delist_code IS NULL
                   OR d.delist_code = ''
                   OR d.delist_reason IS NULL
                   OR d.delist_reason = ''
                   OR d.delisting_return_type IS NULL
                   OR d.delisting_return_type = ''
                   OR d.return_policy IS NULL
                   OR d.return_policy = ''
                   OR d.return_confidence NOT IN ('none', 'low', 'medium', 'high')
                   OR d.evidence_confidence NOT IN ('low', 'medium', 'high')
                   OR d.evidence_source IS NULL
                   OR d.evidence_source = ''
                   OR d.evidence_source_table IS NULL
                   OR d.evidence_source_table = ''
                   OR d.method IS NULL
                   OR d.method = ''
                   OR (
                       d.delisting_return IS NOT NULL
                       AND (d.delisting_return < -1.0 OR d.delisting_return > 10.0)
                   )
                   OR (d.delisting_return IS NOT NULL AND d.return_confidence = 'none')
                   OR (d.is_return_imputed = true AND d.return_policy = 'none')
            """,
            threshold=0.0,
            required_tables=("delisting_events",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="orphan_delisting_event_codes",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                LEFT JOIN delist_code_dim c
                  ON c.delist_code = d.delist_code
                WHERE c.delist_code IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_events", "delist_code_dim"),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="orphan_delisting_event_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                LEFT JOIN securities s
                  ON s.security_id = d.security_id
                WHERE d.security_id IS NOT NULL
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_events", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="orphan_delisting_event_listing_status_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                LEFT JOIN listing_status_intervals l
                  ON l.listing_status_id = d.source_listing_status_id
                WHERE l.listing_status_id IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_events", "listing_status_intervals"),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="orphan_delisting_event_return_observations",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                LEFT JOIN delisting_return_observations o
                  ON o.delisting_return_observation_id = d.return_observation_id
                WHERE d.return_observation_id IS NOT NULL
                  AND o.delisting_return_observation_id IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_events", "delisting_return_observations"),
        ),
        SqlQualityCheck(
            dataset_id="delisting_return_observations",
            table_name="delisting_return_observations",
            check_name="duplicate_delisting_return_observations",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT delisting_return_observation_id, count(*) AS row_count
                    FROM delisting_return_observations
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("delisting_return_observations",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_return_observations",
            table_name="delisting_return_observations",
            check_name="bad_delisting_return_observation_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_return_observations o
                WHERE o.delisting_return_observation_id IS NULL
                   OR o.delisting_return_observation_id = ''
                   OR o.source IS NULL
                   OR o.source = ''
                   OR o.provider IS NULL
                   OR o.provider = ''
                   OR (
                       coalesce(o.security_id, '') = ''
                       AND coalesce(o.symbol, '') = ''
                       AND coalesce(o.vendor_security_id, '') = ''
                   )
                   OR o.delist_date IS NULL
                   OR o.as_of_date IS NULL
                   OR o.available_at IS NULL
                   OR o.delisting_return IS NULL
                   OR o.delisting_return < -1.0
                   OR o.delisting_return > 10.0
                   OR o.return_basis IS NULL
                   OR o.return_basis = ''
            """,
            threshold=0.0,
            required_tables=("delisting_return_observations",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_return_observations",
            table_name="delisting_return_observations",
            check_name="orphan_delisting_return_observation_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_return_observations o
                LEFT JOIN securities s
                  ON s.security_id = o.security_id
                WHERE o.security_id IS NOT NULL
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_return_observations", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_company_facts",
            table_name="fundamental_points",
            check_name="fundamental_period_after_asof",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_points
                WHERE period_end IS NOT NULL
                  AND period_end > as_of_date
            """,
            threshold=0.0,
            required_tables=("fundamental_points",),
        ),
        SqlQualityCheck(
            dataset_id="sec_company_facts",
            table_name="fundamental_points",
            check_name="missing_fundamental_available_at",
            sql="SELECT count(*)::DOUBLE FROM fundamental_points WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("fundamental_points",),
        ),
        SqlQualityCheck(
            dataset_id="sec_company_facts",
            table_name="fundamental_points",
            check_name="duplicate_fundamental_points",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        security_id,
                        metric,
                        coalesce(unit, '') AS unit,
                        period_start,
                        period_end,
                        as_of_date,
                        coalesce(accession_number, '') AS accession_number,
                        count(*) AS row_count
                    FROM fundamental_points
                    GROUP BY 1, 2, 3, 4, 5, 6, 7
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_points",),
        ),
    )
