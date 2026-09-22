-- Prepared read-only desk acceptance; execute only after the guarded materialization.
-- Snapshot is a calendar date, never a claimed trading session. This SELECT emits
-- one aggregate row even when the eligible common-equity universe is empty, then
-- at most 1000 total rows: one aggregate plus 999 deterministic names
-- (300 per disposition plus 99 quarantined overlapping membership names).
WITH pin AS (
    SELECT DATE '2026-09-20' AS snapshot_date,
           TIMESTAMP '2026-09-20 22:00:00' AS cutoff_utc,
           'us_listed_v1'::VARCHAR AS universe_id,
           'atx-db us-listed universe builder'::VARCHAR AS universe_source,
           'atx-db daily market panel v1'::VARCHAR AS market_source,
           'atx-db declarative derived metrics v1'::VARCHAR AS metric_source,
           '1'::VARCHAR AS metric_definition_version,
           200::INTEGER AS max_accounting_age_days
), requested(metric_code, metric_window, expression, registered_inputs_json,
             sorted_inputs_json) AS (
    -- Frozen seed literals and the publisher's quarter-pit-v3-annual fingerprint
    -- ingredients. These four expressions have no direct FY alternative or
    -- weighted-share plan, so repr(alternative)='None', weighted_codes=[].
    VALUES ('eps_diluted_q_growth_yoy', 'q', 'yoy(eps_diluted)',
            '["item:eps_diluted"]', '["item:eps_diluted"]'),
           ('operating_margin_change_yoy', 'ttm',
            'operating_margin - lag(operating_margin, 4)',
            '["metric:operating_margin"]', '["metric:operating_margin"]'),
           ('total_accruals', 'ttm',
            'safe_div(net_income_ttm - cfo_ttm, total_assets_avg2)',
            '["metric:net_income_ttm","metric:cfo_ttm","metric:total_assets_avg2"]',
            '["metric:cfo_ttm","metric:net_income_ttm","metric:total_assets_avg2"]'),
           ('net_debt_ebitda', 'ttm', 'safe_div(net_debt, ebitda_ttm)',
            '["metric:net_debt","metric:ebitda_ttm"]',
            '["metric:ebitda_ttm","metric:net_debt"]')
), expected_hashes AS (
    SELECT r.*,
           sha256('["quarter-pit-v3-annual",' || to_json(r.metric_code) || ','
               || to_json(r.metric_window) || ',' || to_json(r.expression) || ','
               || r.sorted_inputs_json || ',' || to_json(p.metric_definition_version)
               || ',"None",[]]') AS expected_definition_hash
    FROM requested r CROSS JOIN pin p
), hash_contract AS (
    SELECT max(expected_definition_hash) FILTER (
               WHERE metric_code = 'eps_diluted_q_growth_yoy') AS eps_hash,
           max(expected_definition_hash) FILTER (
               WHERE metric_code = 'operating_margin_change_yoy') AS margin_hash,
           max(expected_definition_hash) FILTER (
               WHERE metric_code = 'total_accruals') AS accruals_hash,
           max(expected_definition_hash) FILTER (
               WHERE metric_code = 'net_debt_ebitda') AS leverage_hash
    FROM expected_hashes
), contract AS (
    SELECT count(*) FILTER (WHERE d.metric_code IS NOT NULL
                            AND d.version = p.metric_definition_version
                            AND d.expression = r.expression
                            AND d.inputs_json = r.registered_inputs_json)
               AS matched_definitions
    FROM expected_hashes r CROSS JOIN pin p
    LEFT JOIN derived_metric_definitions d
      ON d.metric_code = r.metric_code AND d.metric_window = r.metric_window
), visible_membership_rows AS (
    SELECT u.*
    FROM universe_us_listed_membership u CROSS JOIN pin p
    WHERE u.universe_id = p.universe_id AND u.source = p.universe_source
      AND u.valid_from <= p.snapshot_date
      AND (u.valid_to IS NULL OR u.valid_to >= p.snapshot_date)
      AND u.available_at <= p.cutoff_utc AND u.as_of_date <= p.snapshot_date
), members AS (
    -- Rank whole membership rows before inspecting class or CIK. The builder
    -- replaces a universe's intervals, so is_latest_revision is not a clock.
    SELECT u.security_id, count(*) AS visible_interval_count,
           count(DISTINCT u.security_type || '|' || u.exchange_code)
               AS distinct_classification_states,
           arg_max(struct_pack(
               membership_id := u.membership_id, symbol := u.symbol,
               security_type := u.security_type, exchange_code := u.exchange_code,
               reason := u.reason, has_cik := u.has_cik, cik := u.cik,
               valid_from := u.valid_from, valid_to := u.valid_to,
               available_at := u.available_at, source := u.source, run_id := u.run_id),
               (u.valid_from, u.available_at, u.membership_id)) AS state
    FROM visible_membership_rows u
    GROUP BY u.security_id
), membership_counts AS (
    SELECT count(*) AS visible_members,
           count(*) FILTER (WHERE state.security_type = 'common'
                            AND state.exchange_code IN ('XNAS','XNYS','XASE','ARCX','BATS'))
               AS visible_common_members,
           count(*) FILTER (WHERE visible_interval_count > 1)
               AS overlapping_membership_securities,
           count(*) FILTER (WHERE visible_interval_count > 1
                            AND state.security_type = 'common'
                            AND state.exchange_code IN ('XNAS','XNYS','XASE','ARCX','BATS'))
               AS quarantined_common_overlaps
    FROM members
), cohort AS (
    SELECT * FROM members
    WHERE visible_interval_count = 1 AND state.security_type = 'common'
      AND state.exchange_code IN ('XNAS','XNYS','XASE','ARCX','BATS')
), observed_session AS (
    SELECT max(m.trade_date) AS trade_date
    FROM market_daily_metrics m CROSS JOIN pin p
    WHERE m.source = p.market_source AND m.trade_date <= p.snapshot_date
      AND m.available_at <= p.cutoff_utc
), market_states AS (
    SELECT m.security_id,
           arg_max(struct_pack(
               market_daily_id := m.market_daily_id, symbol := m.symbol,
               trade_date := m.trade_date, close := m.close,
               market_cap := m.market_cap, earnings_yield := m.earnings_yield,
               cfo_to_ev := m.cfo_to_ev, available_at := m.available_at,
               fundamental_available_at := m.fundamental_available_at,
               inputs_hash := m.inputs_hash, source := m.source, run_id := m.run_id),
               (m.trade_date, m.available_at, m.market_daily_id)) AS state
    FROM market_daily_metrics m JOIN cohort c ON c.security_id = m.security_id
    CROSS JOIN pin p CROSS JOIN observed_session s
    WHERE m.source = p.market_source AND m.trade_date = s.trade_date
      AND m.available_at <= p.cutoff_utc
    GROUP BY m.security_id
), identifier_links AS (
    -- A current ticker directory or securities.entity_id is not historical
    -- issuer qualification. Require a dated, visible CIK identifier interval.
    SELECT h.security_id, count(DISTINCT h.id_value) AS distinct_ciks,
           arg_max(struct_pack(cik := h.id_value, valid_from := h.valid_from,
               valid_to := h.valid_to, available_at := h.available_at,
               source := h.source),
               (h.valid_from, h.available_at, h.id_value)) AS state
    FROM security_identifier_history h JOIN cohort c ON c.security_id = h.security_id
    CROSS JOIN pin p
    WHERE h.id_type = 'CIK' AND h.id_value IS NOT NULL
      AND h.valid_from <= p.snapshot_date
      AND (h.valid_to IS NULL OR h.valid_to > p.snapshot_date)
      AND h.available_at IS NOT NULL AND h.available_at <= p.cutoff_utc
    GROUP BY h.security_id
), ticker_links AS (
    SELECT e.security_id,
           arg_max(struct_pack(ticker := e.ticker, source := e.source,
               valid_from := e.valid_from, available_at := e.available_at),
               (e.valid_from, e.available_at, e.ticker)) AS state
    FROM exchange_listings e JOIN cohort c ON c.security_id = e.security_id
    CROSS JOIN pin p
    WHERE e.valid_from <= p.snapshot_date
      AND (e.valid_to IS NULL OR e.valid_to > p.snapshot_date)
      AND e.available_at IS NOT NULL AND e.available_at <= p.cutoff_utc
      AND e.ticker IS NOT NULL AND e.ticker <> ''
    GROUP BY e.security_id
), metric_states AS (
    -- Preserve NULL/unavailable/conflict rows INSIDE a non-NULL struct. A
    -- value-only arg_max would silently resurrect an older valid value.
    SELECT d.security_id,
           arg_max(struct_pack(value := d.value, value_status := d.value_status,
               history_status := d.history_status, value_origin := d.value_origin,
               period_end := d.period_end, fiscal_period_start := d.fiscal_period_start,
               fiscal_period_end := d.fiscal_period_end, target_bucket := d.target_bucket,
               available_at := d.available_at,
               derived_value_id := d.derived_value_id, inputs_hash := d.inputs_hash,
               definition_hash := d.definition_hash, run_id := d.run_id),
               (d.period_end, d.available_at, d.derived_value_id))
               FILTER (WHERE d.metric_code = 'eps_diluted_q_growth_yoy') AS eps,
           arg_max(struct_pack(value := d.value, value_status := d.value_status,
               history_status := d.history_status, value_origin := d.value_origin,
               period_end := d.period_end, fiscal_period_start := d.fiscal_period_start,
               fiscal_period_end := d.fiscal_period_end, target_bucket := d.target_bucket,
               available_at := d.available_at,
               derived_value_id := d.derived_value_id, inputs_hash := d.inputs_hash,
               definition_hash := d.definition_hash, run_id := d.run_id),
               (d.period_end, d.available_at, d.derived_value_id))
               FILTER (WHERE d.metric_code = 'operating_margin_change_yoy') AS margin,
           arg_max(struct_pack(value := d.value, value_status := d.value_status,
               history_status := d.history_status, value_origin := d.value_origin,
               period_end := d.period_end, fiscal_period_start := d.fiscal_period_start,
               fiscal_period_end := d.fiscal_period_end, target_bucket := d.target_bucket,
               available_at := d.available_at,
               derived_value_id := d.derived_value_id, inputs_hash := d.inputs_hash,
               definition_hash := d.definition_hash, run_id := d.run_id),
               (d.period_end, d.available_at, d.derived_value_id))
               FILTER (WHERE d.metric_code = 'total_accruals') AS accruals,
           arg_max(struct_pack(value := d.value, value_status := d.value_status,
               history_status := d.history_status, value_origin := d.value_origin,
               period_end := d.period_end, fiscal_period_start := d.fiscal_period_start,
               fiscal_period_end := d.fiscal_period_end, target_bucket := d.target_bucket,
               available_at := d.available_at,
               derived_value_id := d.derived_value_id, inputs_hash := d.inputs_hash,
               definition_hash := d.definition_hash, run_id := d.run_id),
               (d.period_end, d.available_at, d.derived_value_id))
               FILTER (WHERE d.metric_code = 'net_debt_ebitda') AS leverage
    FROM derived_metric_values d JOIN cohort c ON c.security_id = d.security_id
    JOIN expected_hashes r ON r.metric_code = d.metric_code
                          AND r.metric_window = d.metric_window
    CROSS JOIN pin p
    WHERE d.source = p.metric_source AND d.period_end <= p.snapshot_date
      AND d.available_at <= p.cutoff_utc
    GROUP BY d.security_id
), eps_owner_evidence AS (
    -- Reconstruct the publisher's selected current EPS input state at the
    -- derived event. Rank ALL CIKs before testing the winning owner's CIK.
    -- inputs_hash is opaque: this does not claim to invert or verify that hash.
    SELECT f.security_id,
           arg_max(struct_pack(security_id := f.security_id, cik := f.cik,
               period_end := f.period_end, basis := f.basis, value := f.value,
               standardized_id := f.standardized_id, source := f.source,
               upstream_source := f.upstream_source,
               source_accession := f.source_accession, filed_date := f.filed_date,
               available_at := f.available_at, rule_id := f.rule_id),
               (f.period_end, f.available_at, f.source, f.rule_id,
                f.basis, f.standardized_id)) AS state
    FROM fundamental_standardized f
    JOIN metric_states d ON d.security_id = f.security_id
      AND CAST(floor((year(f.period_end) * 12 + month(f.period_end) - 1
                       + CASE WHEN day(f.period_end) >= 15 THEN 1 ELSE 0 END)
                     / 3.0) AS BIGINT) = d.eps.target_bucket
    CROSS JOIN pin p
    WHERE f.canonical_code = 'eps_diluted' AND f.basis IN ('quarterly','instant')
      AND f.available_at IS NOT NULL AND f.available_at <= p.cutoff_utc
      AND f.available_at <= d.eps.available_at
      AND f.period_end <= p.snapshot_date
    GROUP BY f.security_id
), joined AS (
    SELECT c.security_id, c.state AS membership, m.state AS market,
           i.state AS identifier, i.distinct_ciks, t.state AS ticker,
           d.eps, d.margin, d.accruals, d.leverage, o.state AS eps_owner
    FROM cohort c LEFT JOIN market_states m USING (security_id)
    LEFT JOIN identifier_links i USING (security_id)
    LEFT JOIN ticker_links t USING (security_id)
    LEFT JOIN metric_states d USING (security_id)
    LEFT JOIN eps_owner_evidence o USING (security_id)
), classified AS (
    SELECT j.*,
           CASE WHEN identifier IS NULL THEN 'missing_dated_cik_join'
                WHEN distinct_ciks <> 1 THEN 'ambiguous_dated_cik_join'
                ELSE 'valid' END AS identity_status,
           CASE WHEN identifier IS NULL OR distinct_ciks <> 1 THEN 'unqualified_identity'
                WHEN eps IS NULL THEN 'missing_eps_metric_state'
                WHEN eps_owner IS NULL THEN 'missing_selected_eps_input_state'
                WHEN eps_owner.cik IS DISTINCT FROM identifier.cik
                    THEN 'selected_eps_owner_mismatch'
                WHEN eps_owner.basis IS DISTINCT FROM 'quarterly'
                    THEN 'selected_eps_input_not_quarterly'
                WHEN eps_owner.period_end IS DISTINCT FROM eps.fiscal_period_end
                    THEN 'selected_eps_period_mismatch'
                ELSE 'valid' END AS eps_owner_status,
           CASE WHEN market IS NULL THEN 'missing_session_market_row'
                WHEN market.close IS NULL OR NOT isfinite(market.close) OR market.close <= 0
                    THEN 'invalid_close'
                WHEN market.market_cap IS NULL OR NOT isfinite(market.market_cap)
                     OR market.market_cap <= 0 THEN 'invalid_market_cap'
                WHEN market.earnings_yield IS NULL OR NOT isfinite(market.earnings_yield)
                    THEN 'missing_earnings_yield'
                WHEN market.cfo_to_ev IS NULL OR NOT isfinite(market.cfo_to_ev)
                    THEN 'missing_cfo_to_ev'
                ELSE 'valid' END AS market_status,
           CASE WHEN eps IS NULL THEN 'missing_metric_state'
                WHEN eps.history_status IS DISTINCT FROM 'event_reconstructed'
                    THEN 'uncertified_history'
                WHEN eps.definition_hash IS DISTINCT FROM hc.eps_hash
                    THEN 'definition_hash_mismatch'
                WHEN eps.value_status IS DISTINCT FROM 'valid' OR eps.value IS NULL
                     OR NOT isfinite(eps.value) THEN 'invalid_current_state'
                WHEN eps.value_origin IS DISTINCT FROM 'quarterly'
                    THEN 'nonquarterly_eps_origin'
                WHEN eps.fiscal_period_end IS NULL
                     OR eps.fiscal_period_end > p.snapshot_date
                    THEN 'missing_operand_period'
                WHEN date_diff('day', eps.fiscal_period_end, p.snapshot_date)
                     > p.max_accounting_age_days
                    THEN 'stale_period' ELSE 'valid' END AS eps_status,
           CASE WHEN margin IS NULL THEN 'missing_metric_state'
                WHEN margin.history_status IS DISTINCT FROM 'event_reconstructed'
                    THEN 'uncertified_history'
                WHEN margin.definition_hash IS DISTINCT FROM hc.margin_hash
                    THEN 'definition_hash_mismatch'
                WHEN margin.value_status IS DISTINCT FROM 'valid' OR margin.value IS NULL
                     OR NOT isfinite(margin.value) THEN 'invalid_current_state'
                WHEN margin.fiscal_period_end IS NULL
                     OR margin.fiscal_period_end > p.snapshot_date
                    THEN 'missing_operand_period'
                WHEN date_diff('day', margin.fiscal_period_end, p.snapshot_date)
                     > p.max_accounting_age_days
                    THEN 'stale_period' ELSE 'valid' END AS margin_status,
           CASE WHEN accruals IS NULL THEN 'missing_metric_state'
                WHEN accruals.history_status IS DISTINCT FROM 'event_reconstructed'
                    THEN 'uncertified_history'
                WHEN accruals.definition_hash IS DISTINCT FROM hc.accruals_hash
                    THEN 'definition_hash_mismatch'
                WHEN accruals.value_status IS DISTINCT FROM 'valid' OR accruals.value IS NULL
                     OR NOT isfinite(accruals.value) THEN 'invalid_current_state'
                WHEN accruals.fiscal_period_end IS NULL
                     OR accruals.fiscal_period_end > p.snapshot_date
                    THEN 'missing_operand_period'
                WHEN date_diff('day', accruals.fiscal_period_end, p.snapshot_date)
                     > p.max_accounting_age_days
                    THEN 'stale_period' ELSE 'valid' END AS accruals_status,
           CASE WHEN leverage IS NULL THEN 'missing_metric_state'
                WHEN leverage.history_status IS DISTINCT FROM 'event_reconstructed'
                    THEN 'uncertified_history'
                WHEN leverage.definition_hash IS DISTINCT FROM hc.leverage_hash
                    THEN 'definition_hash_mismatch'
                WHEN leverage.value_status IS DISTINCT FROM 'valid' OR leverage.value IS NULL
                     OR NOT isfinite(leverage.value) THEN 'invalid_current_state'
                WHEN leverage.fiscal_period_end IS NULL
                     OR leverage.fiscal_period_end > p.snapshot_date
                    THEN 'missing_operand_period'
                WHEN date_diff('day', leverage.fiscal_period_end, p.snapshot_date)
                     > p.max_accounting_age_days
                    THEN 'stale_period' ELSE 'valid' END AS leverage_status
    FROM joined j CROSS JOIN pin p CROSS JOIN hash_contract hc
), disposition AS (
    SELECT *,
           CASE WHEN ct.matched_definitions = 4
                 AND identity_status = 'valid' AND eps_owner_status = 'valid'
                 AND market_status = 'valid'
                 AND eps_status = 'valid' AND margin_status = 'valid'
                 AND accruals_status = 'valid' AND leverage_status = 'valid'
                THEN true ELSE false END AS complete
    FROM classified CROSS JOIN contract ct
), named AS (
    SELECT *, CASE WHEN NOT complete THEN 'incomplete'
                   WHEN eps.value > 0 AND margin.value > 0 THEN 'passes_descriptive_filter'
                   ELSE 'complete_not_improving' END AS screen_status
    FROM disposition
), totals AS (
    SELECT count(*) AS common_rows,
           count(*) FILTER (WHERE identity_status = 'valid') AS qualified_issuer_joins,
           count(*) FILTER (WHERE identity_status <> 'valid') AS missing_security_joins,
           count(*) FILTER (WHERE eps_owner_status = 'valid') AS qualified_eps_owners,
           count(*) FILTER (WHERE eps_owner_status = 'missing_selected_eps_input_state')
               AS missing_eps_owner_evidence,
           count(*) FILTER (WHERE eps_owner_status = 'selected_eps_owner_mismatch')
               AS selected_eps_owner_mismatch,
           count(*) FILTER (WHERE eps_owner_status = 'selected_eps_input_not_quarterly'
                            OR eps_owner_status = 'selected_eps_period_mismatch')
               AS selected_eps_input_contract_failure,
           count(*) FILTER (WHERE ticker IS NULL) AS missing_qualified_ticker_display,
           count(*) FILTER (WHERE market IS NOT NULL
                            AND market.close IS NOT NULL AND isfinite(market.close)
                            AND market.close > 0) AS usable_observed_price,
           count(*) FILTER (WHERE market IS NOT NULL
                            AND market.market_cap IS NOT NULL AND isfinite(market.market_cap)
                            AND market.market_cap > 0) AS usable_market_cap,
           count(*) FILTER (WHERE market IS NOT NULL
                            AND market.earnings_yield IS NOT NULL
                            AND isfinite(market.earnings_yield)) AS usable_earnings_yield,
           count(*) FILTER (WHERE market IS NOT NULL
                            AND market.cfo_to_ev IS NOT NULL
                            AND isfinite(market.cfo_to_ev)) AS usable_cfo_to_ev,
           count(*) FILTER (WHERE market_status = 'valid') AS usable_price_valuation,
           count(*) FILTER (WHERE market_status = 'missing_session_market_row') AS missing_price_row,
           count(*) FILTER (WHERE market IS NULL OR market.close IS NULL
                            OR NOT isfinite(market.close) OR market.close <= 0)
               AS invalid_or_missing_close,
           count(*) FILTER (WHERE market IS NULL OR market.market_cap IS NULL
                            OR NOT isfinite(market.market_cap) OR market.market_cap <= 0)
               AS invalid_or_missing_market_cap,
           count(*) FILTER (WHERE market IS NULL OR market.earnings_yield IS NULL
                            OR NOT isfinite(market.earnings_yield)) AS missing_earnings_yield,
           count(*) FILTER (WHERE market IS NULL OR market.cfo_to_ev IS NULL
                            OR NOT isfinite(market.cfo_to_ev)) AS missing_cfo_to_ev,
           count(*) FILTER (WHERE eps_status = 'valid') AS usable_eps_growth,
           count(*) FILTER (WHERE margin_status = 'valid') AS usable_margin_change,
           count(*) FILTER (WHERE accruals_status = 'valid') AS usable_accruals,
           count(*) FILTER (WHERE leverage_status = 'valid') AS usable_leverage,
           count(*) FILTER (WHERE eps_status = 'missing_metric_state') AS missing_eps_state,
           count(*) FILTER (WHERE margin_status = 'missing_metric_state') AS missing_margin_state,
           count(*) FILTER (WHERE accruals_status = 'missing_metric_state') AS missing_accruals_state,
           count(*) FILTER (WHERE leverage_status = 'missing_metric_state') AS missing_leverage_state,
           count(*) FILTER (WHERE (eps IS NOT NULL AND
                                   (eps.value_status IS DISTINCT FROM 'valid'
                                    OR eps.value IS NULL OR NOT isfinite(eps.value)))
                            OR (margin IS NOT NULL AND
                                   (margin.value_status IS DISTINCT FROM 'valid'
                                    OR margin.value IS NULL OR NOT isfinite(margin.value)))
                            OR (accruals IS NOT NULL AND
                                   (accruals.value_status IS DISTINCT FROM 'valid'
                                    OR accruals.value IS NULL OR NOT isfinite(accruals.value)))
                            OR (leverage IS NOT NULL AND
                                   (leverage.value_status IS DISTINCT FROM 'valid'
                                    OR leverage.value IS NULL OR NOT isfinite(leverage.value))))
               AS any_invalid_current_state,
           count(*) FILTER (WHERE (eps IS NOT NULL AND eps.fiscal_period_end IS NOT NULL
                                   AND date_diff('day', eps.fiscal_period_end,
                                                 p.snapshot_date) > p.max_accounting_age_days)
                            OR (margin IS NOT NULL AND margin.fiscal_period_end IS NOT NULL
                                   AND date_diff('day', margin.fiscal_period_end,
                                                 p.snapshot_date) > p.max_accounting_age_days)
                            OR (accruals IS NOT NULL AND accruals.fiscal_period_end IS NOT NULL
                                   AND date_diff('day', accruals.fiscal_period_end,
                                                 p.snapshot_date) > p.max_accounting_age_days)
                            OR (leverage IS NOT NULL AND leverage.fiscal_period_end IS NOT NULL
                                   AND date_diff('day', leverage.fiscal_period_end,
                                                 p.snapshot_date) > p.max_accounting_age_days))
               AS any_stale_period,
           count(*) FILTER (WHERE (eps IS NOT NULL AND eps.history_status
                                   IS DISTINCT FROM 'event_reconstructed')
                            OR (margin IS NOT NULL AND margin.history_status
                                   IS DISTINCT FROM 'event_reconstructed')
                            OR (accruals IS NOT NULL AND accruals.history_status
                                   IS DISTINCT FROM 'event_reconstructed')
                            OR (leverage IS NOT NULL AND leverage.history_status
                                   IS DISTINCT FROM 'event_reconstructed'))
               AS any_uncertified_history,
           count(*) FILTER (WHERE eps_status = 'nonquarterly_eps_origin') AS nonquarterly_eps_origin,
           count(*) FILTER (WHERE (eps IS NOT NULL AND eps.definition_hash
                                   IS DISTINCT FROM hc.eps_hash)
                            OR (margin IS NOT NULL AND margin.definition_hash
                                   IS DISTINCT FROM hc.margin_hash)
                            OR (accruals IS NOT NULL AND accruals.definition_hash
                                   IS DISTINCT FROM hc.accruals_hash)
                            OR (leverage IS NOT NULL AND leverage.definition_hash
                                   IS DISTINCT FROM hc.leverage_hash))
               AS any_definition_hash_mismatch,
           count(*) FILTER (WHERE (eps IS NOT NULL AND
                                   (eps.fiscal_period_end IS NULL
                                    OR eps.fiscal_period_end > p.snapshot_date))
                            OR (margin IS NOT NULL AND
                                   (margin.fiscal_period_end IS NULL
                                    OR margin.fiscal_period_end > p.snapshot_date))
                            OR (accruals IS NOT NULL AND
                                   (accruals.fiscal_period_end IS NULL
                                    OR accruals.fiscal_period_end > p.snapshot_date))
                            OR (leverage IS NOT NULL AND
                                   (leverage.fiscal_period_end IS NULL
                                    OR leverage.fiscal_period_end > p.snapshot_date)))
               AS any_missing_operand_period,
           count(*) FILTER (WHERE complete) AS complete_rows,
           count(*) FILTER (WHERE screen_status = 'passes_descriptive_filter') AS filter_passes,
           count(*) FILTER (WHERE screen_status = 'complete_not_improving') AS filter_exclusions,
           count(*) FILTER (WHERE screen_status = 'incomplete') AS incomplete_rows
    FROM named CROSS JOIN pin p CROSS JOIN hash_contract hc
), preview AS (
    SELECT *, row_number() OVER (
        PARTITION BY screen_status
        ORDER BY market.market_cap DESC NULLS LAST, security_id
    ) AS preview_rank
    FROM named
), overlap_preview AS (
    SELECT *, row_number() OVER (ORDER BY security_id) AS preview_rank
    FROM members WHERE visible_interval_count > 1
)
SELECT 'aggregate' AS row_type, p.snapshot_date, p.cutoff_utc,
       s.trade_date AS observed_trade_date, NULL::VARCHAR AS security_id,
       NULL::VARCHAR AS qualified_ticker,
       to_json(struct_pack(
           status := CASE WHEN mc.visible_members = 0 THEN 'empty_membership'
                          WHEN mc.overlapping_membership_securities > 0
                              THEN 'membership_overlap_quarantined'
                          WHEN mc.visible_common_members = 0 THEN 'empty_common_universe'
                          WHEN ct.matched_definitions <> 4 THEN 'metric_definition_contract_missing'
                          WHEN s.trade_date IS NULL THEN 'no_visible_market_session'
                          WHEN t.any_definition_hash_mismatch > 0
                              THEN 'definition_hash_mismatch'
                          WHEN t.selected_eps_owner_mismatch > 0
                              THEN 'selected_eps_owner_mismatch'
                          ELSE 'measured' END,
           universe_id := p.universe_id, universe_source := p.universe_source,
           market_source := p.market_source, metric_source := p.metric_source,
           metric_definition_version := p.metric_definition_version,
           max_accounting_age_days := p.max_accounting_age_days,
           matched_definitions := ct.matched_definitions,
           visible_members := mc.visible_members,
           visible_common_members := mc.visible_common_members,
           overlapping_membership_securities := mc.overlapping_membership_securities,
           quarantined_common_overlaps := mc.quarantined_common_overlaps,
           common_rows := t.common_rows,
           qualified_issuer_joins := t.qualified_issuer_joins,
           missing_security_joins := t.missing_security_joins,
           qualified_eps_owners := t.qualified_eps_owners,
           missing_eps_owner_evidence := t.missing_eps_owner_evidence,
           selected_eps_owner_mismatch := t.selected_eps_owner_mismatch,
           selected_eps_input_contract_failure := t.selected_eps_input_contract_failure,
           missing_qualified_ticker_display := t.missing_qualified_ticker_display,
           usable_observed_price := t.usable_observed_price,
           usable_market_cap := t.usable_market_cap,
           usable_earnings_yield := t.usable_earnings_yield,
           usable_cfo_to_ev := t.usable_cfo_to_ev,
           usable_price_valuation := t.usable_price_valuation,
           missing_price_row := t.missing_price_row,
           invalid_or_missing_close := t.invalid_or_missing_close,
           invalid_or_missing_market_cap := t.invalid_or_missing_market_cap,
           missing_earnings_yield := t.missing_earnings_yield,
           missing_cfo_to_ev := t.missing_cfo_to_ev,
           usable_eps_growth := t.usable_eps_growth,
           usable_margin_change := t.usable_margin_change,
           usable_accruals := t.usable_accruals,
           usable_leverage := t.usable_leverage,
           missing_eps_state := t.missing_eps_state,
           missing_margin_state := t.missing_margin_state,
           missing_accruals_state := t.missing_accruals_state,
           missing_leverage_state := t.missing_leverage_state,
           any_invalid_current_state := t.any_invalid_current_state,
           any_stale_period := t.any_stale_period,
           any_uncertified_history := t.any_uncertified_history,
           nonquarterly_eps_origin := t.nonquarterly_eps_origin,
           any_definition_hash_mismatch := t.any_definition_hash_mismatch,
           any_missing_operand_period := t.any_missing_operand_period,
           complete_rows := t.complete_rows,
           filter_passes := t.filter_passes,
           filter_exclusions := t.filter_exclusions,
           incomplete_rows := t.incomplete_rows)) AS details_json,
       0 AS display_order
FROM pin p CROSS JOIN observed_session s CROSS JOIN membership_counts mc
CROSS JOIN contract ct CROSS JOIN totals t
UNION ALL
SELECT 'name', p.snapshot_date, p.cutoff_utc, s.trade_date,
       n.security_id, n.ticker.ticker,
       to_json(struct_pack(
           screen_status := n.screen_status, complete := n.complete,
           matched_definitions := ct.matched_definitions,
           identity_status := n.identity_status, eps_owner_status := n.eps_owner_status,
           market_status := n.market_status,
           eps_status := n.eps_status, margin_status := n.margin_status,
           accruals_status := n.accruals_status, leverage_status := n.leverage_status,
           expected_definition_hashes := struct_pack(
               eps := hc.eps_hash, margin := hc.margin_hash,
               accruals := hc.accruals_hash, leverage := hc.leverage_hash),
           accounting_age_days := struct_pack(
               eps_target := date_diff('day', n.eps.period_end, p.snapshot_date),
               eps_operand := date_diff('day', n.eps.fiscal_period_end, p.snapshot_date),
               margin_target := date_diff('day', n.margin.period_end, p.snapshot_date),
               margin_operand := date_diff('day', n.margin.fiscal_period_end, p.snapshot_date),
               accruals_target := date_diff('day', n.accruals.period_end, p.snapshot_date),
               accruals_operand := date_diff('day', n.accruals.fiscal_period_end, p.snapshot_date),
               leverage_target := date_diff('day', n.leverage.period_end, p.snapshot_date),
               leverage_operand := date_diff('day', n.leverage.fiscal_period_end, p.snapshot_date)),
           membership := n.membership, dated_cik_link := n.identifier,
           reported_eps_owner := n.eps_owner,
           distinct_dated_ciks := n.distinct_ciks, qualified_ticker_link := n.ticker,
           market := n.market, eps_growth := n.eps,
           margin_change := n.margin, total_accruals := n.accruals,
           net_debt_ebitda := n.leverage)) AS details_json,
       CASE n.screen_status WHEN 'passes_descriptive_filter' THEN 1
            WHEN 'complete_not_improving' THEN 2 ELSE 3 END AS display_order
FROM preview n CROSS JOIN pin p CROSS JOIN observed_session s
CROSS JOIN contract ct CROSS JOIN hash_contract hc
WHERE n.preview_rank <= 300
UNION ALL
SELECT 'membership_ambiguous', p.snapshot_date, p.cutoff_utc, s.trade_date,
       o.security_id, NULL::VARCHAR,
       to_json(struct_pack(status := 'overlapping_membership_quarantined',
           visible_interval_count := o.visible_interval_count,
           distinct_classification_states := o.distinct_classification_states,
           highest_ranked_membership_for_diagnosis_only := o.state)),
       4 AS display_order
FROM overlap_preview o CROSS JOIN pin p CROSS JOIN observed_session s
WHERE o.preview_rank <= 99
ORDER BY display_order, security_id;
