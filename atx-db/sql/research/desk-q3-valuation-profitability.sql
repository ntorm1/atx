-- Q3: same-session valuation/profitability cross-section, USD market-cap floor.
-- Parameters: cutoff TIMESTAMP, market_cap_floor DOUBLE.
-- Dated CIK bridge follows FQ1 interval conventions; source owners are never
-- equated to trading security IDs. ADRs and non-common types remain diagnostics.
-- Share basis (A8 market_daily_metrics.shares_source) and owner link (A5 labels,
-- migration 0327) are exposed on every row. market_cap and every multiple are
-- shown only under a named share basis:
--   verified_dei_shares      dei: the owner's DEI count, known at its filing clock;
--   unverified_vendor_shares archive / archive_ads / archive_split_adjusted /
--                            class_sum: vendor counts on the modeled vendor-run
--                            clock (class_sum = multi-class issuer cap, sum of
--                            close_i x class count_i; archive_ads = ADS basis).
-- Withheld bases (multiclass_unresolved, adr_ratio_unresolved,
-- dei_archive_conflict, split_unresolved) and rows without a share-basis label
-- never show a market cap or multiple, whatever the stored row holds.
-- The market row's owner (A5 bridge) must be this reader's dated-CIK issuer:
-- CIKs are compared when the owner id carries one (SEC-CIK-X, *-CIK-X member
-- ids), else the owner ids. Requires schema >= 0327 (identity label columns).
-- One line per CIK is ranked (highest same-session close x volume, then
-- security_id); other candidate lines of that CIK are secondary_issuer_line.
-- Reporting currency (A8 guard) is not stored per row: a currency-withheld row
-- keeps market_cap and surfaces as valuation_nonpositive_denominator_or_missing.
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
), market_labeled AS (
    SELECT m.*,
      CASE WHEN m.shares_source='dei' THEN 'verified_dei_shares'
           WHEN m.shares_source IN ('archive','archive_ads','archive_split_adjusted','class_sum')
             THEN 'unverified_vendor_shares'
           WHEN m.shares_source IN ('multiclass_unresolved','adr_ratio_unresolved',
                                    'dei_archive_conflict','split_unresolved') THEN 'withheld'
           ELSE 'unlabeled' END AS share_basis_status
    FROM market_ranked m WHERE m.state_rank=1
), market AS (
    SELECT m.security_id,m.symbol,m.market_daily_id,m.available_at AS market_available_at,
      m.shares_source,m.share_basis_status,
      CASE m.share_basis_status WHEN 'verified_dei_shares' THEN 'filing_available_at'
           WHEN 'unverified_vendor_shares' THEN 'vendor_run_clock' END AS shares_availability_basis,
      m.owner_security_id AS market_owner_security_id,
      nullif(regexp_extract(m.owner_security_id,'CIK-([0-9]{10})$',1),'') AS market_owner_cik,m.identity_basis,
      m.availability_basis AS owner_link_availability_basis,m.link_method,
      m.close*m.volume AS dollar_volume,
      m.share_basis_status IN ('verified_dei_shares','unverified_vendor_shares') AS basis_shown,
      CASE WHEN basis_shown THEN m.shares_outstanding END AS shares_outstanding,
      CASE WHEN basis_shown THEN m.shares_reconciliation_ratio END AS shares_reconciliation_ratio,
      CASE WHEN basis_shown THEN m.market_cap END AS market_cap,
      CASE WHEN basis_shown THEN m.pe_ttm END AS pe_ttm,
      CASE WHEN basis_shown THEN m.ev_ebitda END AS ev_ebitda,
      CASE WHEN basis_shown THEN m.pb END AS pb,
      CASE WHEN basis_shown THEN m.fcf_yield END AS fcf_yield,
      CASE WHEN basis_shown AND m.ebit_to_ev>0 AND isfinite(m.ebit_to_ev) AND m.enterprise_value>0
           THEN 1.0/m.ebit_to_ev END AS ev_ebit
    FROM market_labeled m
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
      m.* EXCLUDE (security_id,basis_shown),
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
           WHEN m.market_daily_id IS NULL THEN 'market_row_missing'
           WHEN m.share_basis_status='withheld' THEN 'share_basis_withheld'
           WHEN m.share_basis_status='unlabeled' THEN 'share_basis_unlabeled'
           WHEN m.market_owner_security_id IS NULL OR m.identity_basis IS NULL THEN 'market_owner_link_unlabeled'
           WHEN CASE WHEN m.market_owner_cik IS NOT NULL THEN m.market_owner_cik<>i.cik
                     ELSE m.market_owner_security_id<>o.security_id END THEN 'market_owner_mismatch'
           WHEN m.market_cap IS NULL OR NOT isfinite(m.market_cap) OR m.market_cap<p.floor THEN 'market_cap_floor_or_missing'
           WHEN f.oldest_fiscal_end IS NULL OR f.fiscal_endpoints<>1
             OR date_diff('day',f.oldest_fiscal_end,c.trade_date)>200 THEN 'profitability_stale_or_mixed'
           WHEN f.roic IS NULL OR f.roe IS NULL OR f.gross_profitability IS NULL THEN 'profitability_missing'
           WHEN m.pe_ttm IS NULL OR NOT isfinite(m.pe_ttm) OR m.pe_ttm<=0
             OR m.ev_ebitda IS NULL OR NOT isfinite(m.ev_ebitda) OR m.ev_ebitda<=0
             OR m.pb IS NULL OR NOT isfinite(m.pb) OR m.pb<=0
             OR m.fcf_yield IS NULL OR NOT isfinite(m.fcf_yield)
             OR m.ev_ebit IS NULL THEN 'valuation_nonpositive_denominator_or_missing'
           ELSE 'candidate_complete' END AS status
    FROM members u CROSS JOIN clock c CROSS JOIN p LEFT JOIN ids i USING (security_id)
    LEFT JOIN owners o ON o.cik=i.cik AND o.owners_per_cik=1 AND o.ciks_per_owner=1
    LEFT JOIN market m ON m.security_id=u.security_id
    LEFT JOIN profit f ON f.security_id=o.security_id
), issuer_lines AS (
    -- One ranked line per issuer: class lines of a multi-class issuer carry the
    -- same issuer cap and fundamentals and must not be ranked twice.
    SELECT security_id,
      row_number() OVER (PARTITION BY cik ORDER BY dollar_volume DESC NULLS LAST,security_id) AS issuer_line_rank,
      count(*) OVER (PARTITION BY cik) AS issuer_candidate_lines
    FROM joined WHERE status='candidate_complete'
), screened AS (
    SELECT j.* REPLACE (CASE WHEN l.issuer_line_rank>1 THEN 'secondary_issuer_line' ELSE j.status END AS status),
      l.issuer_candidate_lines
    FROM joined j LEFT JOIN issuer_lines l USING (security_id)
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
      count(*) OVER () AS ranked_names,
      count(*) FILTER (WHERE share_basis_status='unverified_vendor_shares') OVER () AS ranked_unverified_share_basis
    FROM screened WHERE status='candidate_complete'
    QUALIFY ranked_names>=10
)
SELECT j.*,d.* EXCLUDE (security_id),count(*) OVER () AS visible_members,
       false AS production_qualified,
       'requires_selected_leaf_proof_row_level_USD_currency_verified_share_basis_and_dated_identity_certification'
           AS qualification
FROM screened j LEFT JOIN deciles d USING (security_id) ORDER BY j.security_id;
