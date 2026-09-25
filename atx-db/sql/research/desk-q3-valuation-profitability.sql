-- Q3: same-session valuation/profitability cross-section, USD market-cap floor.
-- Parameters: cutoff TIMESTAMP, market_cap_floor DOUBLE.
-- Dated CIK bridge follows FQ1 interval conventions; source owners are never
-- equated to trading security IDs. ADRs and non-common types remain diagnostics.
-- Deciles are descriptive, 1=lowest, with security_id tie-breaks. Rank the full
-- complete-case cohort before the runner's output cap. No certified market PIT.
WITH p AS (SELECT $cutoff::TIMESTAMP AS cutoff,$market_cap_floor::DOUBLE AS floor),
session AS (
    -- Same observed-session clock as FQ1: a row published after its session's
    -- 22:00 decision clock cannot define the session (else no row survives below).
    SELECT max(trade_date) AS trade_date FROM market_daily_metrics,p
    WHERE source='atx-db daily market panel v1' AND available_at<=p.cutoff
      AND as_of_date<=p.cutoff::DATE AND trade_date<=p.cutoff::DATE
      AND available_at<=trade_date::TIMESTAMP+INTERVAL 22 HOUR
), clock AS (
    SELECT trade_date,least(p.cutoff,trade_date::TIMESTAMP+INTERVAL 22 HOUR) AS cutoff
    FROM session,p
), members AS (
    SELECT u.security_id,max(u.security_type) AS security_type,max(u.exchange_code) AS exchange_code,
           max(u.cik) AS membership_cik,count(*) AS membership_rows
    FROM universe_us_listed_membership u,clock c
    WHERE u.universe_id='us_listed_v1' AND u.source='atx-db us-listed universe builder'
      AND u.valid_from<=c.trade_date AND (u.valid_to IS NULL OR u.valid_to>=c.trade_date)
      AND u.available_at<=c.cutoff AND u.as_of_date<=c.trade_date GROUP BY u.security_id
), ids AS (
    SELECT h.security_id,count(DISTINCT CASE WHEN regexp_full_match(h.id_value,'[0-9]{1,10}')
             THEN lpad(h.id_value,10,'0') ELSE h.id_value END) AS cik_count,
           max(CASE WHEN regexp_full_match(h.id_value,'[0-9]{1,10}') THEN lpad(h.id_value,10,'0') END) AS cik,
           count(*) FILTER (WHERE NOT regexp_full_match(h.id_value,'[0-9]{1,10}') OR h.id_value IS NULL) AS invalid_ids
    FROM security_identifier_history h JOIN members USING (security_id) CROSS JOIN clock c
    WHERE h.id_type='CIK' AND h.valid_from<=c.trade_date
      AND (h.valid_to IS NULL OR h.valid_to>c.trade_date)
      AND h.available_at<=c.cutoff AND h.as_of_date<=c.trade_date GROUP BY h.security_id
), owner_pairs AS (
    SELECT DISTINCT f.security_id,lpad(f.cik,10,'0') AS cik
    FROM fundamental_fact_revisions f,clock c
    WHERE regexp_full_match(f.cik,'[0-9]{1,10}')
      AND f.available_at<=c.cutoff AND f.as_of_date<=c.trade_date
), owners AS (
    SELECT security_id,cik,count(*) OVER (PARTITION BY cik) AS owners_per_cik,
           count(*) OVER (PARTITION BY security_id) AS ciks_per_owner FROM owner_pairs
), market_ranked AS (
    SELECT m.*,row_number() OVER (PARTITION BY m.security_id
        ORDER BY m.available_at DESC,m.market_daily_id DESC) AS state_rank
    FROM market_daily_metrics m,clock c
    WHERE m.source='atx-db daily market panel v1' AND m.trade_date=c.trade_date
      AND m.available_at<=c.cutoff AND m.as_of_date<=c.trade_date
), derived_ranked AS (
    SELECT d.*,row_number() OVER (PARTITION BY security_id,metric_code,metric_window,target_bucket
        ORDER BY available_at DESC,derived_value_id DESC) AS state_rank
    FROM derived_metric_values d,clock c
    WHERE d.source='atx-db declarative derived metrics v1' AND d.metric_window='ttm'
      AND d.metric_code IN ('roic','roe','gross_profitability')
      AND d.available_at<=c.cutoff AND d.as_of_date<=c.trade_date AND d.period_end<=c.trade_date
), latest AS (
    SELECT *,row_number() OVER (PARTITION BY security_id,metric_code
        ORDER BY target_bucket DESC,available_at DESC,derived_value_id DESC) AS period_rank
    FROM derived_ranked WHERE state_rank=1
), profit AS (
    SELECT security_id,
      max(CASE WHEN metric_code='roic' AND value_status='valid' AND isfinite(value) THEN value END) AS roic,
      max(CASE WHEN metric_code='roe' AND value_status='valid' AND isfinite(value) THEN value END) AS roe,
      max(CASE WHEN metric_code='gross_profitability' AND value_status='valid' AND isfinite(value) THEN value END) AS gross_profitability,
      count(DISTINCT fiscal_period_end) AS fiscal_endpoints,min(fiscal_period_end) AS oldest_fiscal_end,
      list(struct_pack(metric := metric_code,state_id := derived_value_id,status := value_status,
          available_at := available_at,refs_hash := selected_input_refs_hash,history := history_status)) AS input_states
    FROM latest WHERE period_rank=1 GROUP BY security_id
), joined AS (
    SELECT u.security_id,i.cik,o.security_id AS issuer_owner_id,c.trade_date,c.cutoff AS decision_at,
      date_diff('day',c.trade_date,p.cutoff::DATE) AS market_session_lag_days,
      m.symbol,m.market_daily_id,m.available_at AS market_available_at,m.market_cap,
      m.pe_ttm,m.ev_ebitda,m.pb,m.fcf_yield,
      CASE WHEN m.ebit_to_ev>0 AND isfinite(m.ebit_to_ev) AND m.enterprise_value>0
           THEN 1.0/m.ebit_to_ev END AS ev_ebit,
      f.roic,f.roe,f.gross_profitability,f.oldest_fiscal_end,f.input_states,
      CASE WHEN u.membership_rows<>1 THEN 'overlapping_membership'
           WHEN u.security_type IS DISTINCT FROM 'common'
             OR u.exchange_code NOT IN ('XNAS','XNYS','XASE','ARCX','BATS')
             OR u.exchange_code IS NULL THEN 'outside_US_common_contract'
           WHEN i.cik_count IS DISTINCT FROM 1 OR i.invalid_ids<>0 OR i.cik IS NULL THEN 'dated_CIK_unresolved'
           WHEN u.membership_cik IS NOT NULL AND
             (NOT regexp_full_match(u.membership_cik,'[0-9]{1,10}') OR lpad(u.membership_cik,10,'0')<>i.cik)
             THEN 'membership_CIK_mismatch'
           WHEN o.security_id IS NULL THEN 'issuer_owner_unresolved_or_ambiguous'
           WHEN m.market_cap IS NULL OR NOT isfinite(m.market_cap) OR m.market_cap<p.floor THEN 'market_cap_floor_or_missing'
           WHEN f.oldest_fiscal_end IS NULL OR f.fiscal_endpoints<>1
             OR date_diff('day',f.oldest_fiscal_end,c.trade_date)>200 THEN 'profitability_stale_or_mixed'
           WHEN f.roic IS NULL OR f.roe IS NULL OR f.gross_profitability IS NULL THEN 'profitability_missing'
           WHEN m.pe_ttm IS NULL OR NOT isfinite(m.pe_ttm) OR m.pe_ttm<=0
             OR m.ev_ebitda IS NULL OR NOT isfinite(m.ev_ebitda) OR m.ev_ebitda<=0
             OR m.pb IS NULL OR NOT isfinite(m.pb) OR m.pb<=0
             OR m.fcf_yield IS NULL OR NOT isfinite(m.fcf_yield)
             OR ev_ebit IS NULL THEN 'valuation_nonpositive_denominator_or_missing'
           ELSE 'candidate_complete' END AS status
    FROM members u CROSS JOIN clock c CROSS JOIN p LEFT JOIN ids i USING (security_id)
    LEFT JOIN owners o ON o.cik=i.cik AND o.owners_per_cik=1 AND o.ciks_per_owner=1
    LEFT JOIN market_ranked m ON m.security_id=u.security_id AND m.state_rank=1
    LEFT JOIN profit f ON f.security_id=o.security_id
), deciles AS (
    SELECT security_id,
      ntile(10) OVER (ORDER BY pe_ttm,security_id) AS pe_decile,
      ntile(10) OVER (ORDER BY ev_ebit,security_id) AS ev_ebit_decile,
      ntile(10) OVER (ORDER BY ev_ebitda,security_id) AS ev_ebitda_decile,
      ntile(10) OVER (ORDER BY pb,security_id) AS pb_decile,
      ntile(10) OVER (ORDER BY fcf_yield,security_id) AS fcf_yield_decile,
      ntile(10) OVER (ORDER BY roic,security_id) AS roic_decile,
      ntile(10) OVER (ORDER BY roe,security_id) AS roe_decile,
      ntile(10) OVER (ORDER BY gross_profitability,security_id) AS gross_profitability_decile,
      count(*) OVER () AS ranked_names
    FROM joined WHERE status='candidate_complete'
    QUALIFY ranked_names>=10
)
SELECT j.*,d.* EXCLUDE (security_id),count(*) OVER () AS visible_members,
       false AS production_qualified,
       'requires_selected_leaf_proof_USD_currency_and_share_class_share_count_PIT_certification' AS qualification
FROM joined j LEFT JOIN deciles d USING (security_id) ORDER BY j.security_id;
