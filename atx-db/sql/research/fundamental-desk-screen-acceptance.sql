-- Inspection only: the runner must first validate this exact completed FQ1 run
-- in the same read-only transaction. This SQL alone does not certify its digest.
-- Parameters: build run ID, report as-of DATE. One diagnostic plus <=999 names.
WITH pin AS (
  SELECT ?::VARCHAR AS run_id, ?::DATE AS report_as_of
), selected_session AS (
  SELECT max(c.decision_date) AS decision_date
  FROM fundamental_signal_coverage c, pin p
  WHERE c.run_id=p.run_id AND c.decision_date<=p.report_as_of
), coverage AS (
  SELECT max(c.decision_at) AS decision_at, max(c.visible_members) AS visible_members,
    max(c.common_members) AS common_members, max(c.overlap_members) AS overlap_members,
    max(c.qualified_cik) AS qualified_cik,
    max(c.status) FILTER (WHERE c.signal_id='eps_growth_yoy') AS eps_status
  FROM fundamental_signal_coverage c, pin p, selected_session s
  WHERE c.run_id=p.run_id AND c.decision_date=s.decision_date
), cohort AS (
  SELECT DISTINCT v.security_id
  FROM fundamental_signal_values v, pin p, selected_session s
  WHERE v.run_id=p.run_id AND v.decision_date=s.decision_date
    AND v.signal_id IN ('eps_growth_yoy','operating_margin_change_yoy',
      'low_total_accruals','low_net_debt_ebitda')
), legs AS (
  SELECT v.security_id,v.signal_id,v.eligible,v.reason AS score_reason,v.score,
    i.raw_value,i.reason AS input_reason,i.derived_value_id,
    i.derived_owner_security_id,i.selected_input_cik,i.lineage_status,
    i.lineage_digest,i.period_end,i.fiscal_period_start,i.fiscal_period_end,
    i.available_at,i.selected_leaf_oldest_end,i.selected_leaf_newest_end
  FROM fundamental_signal_values v JOIN fundamental_signal_inputs i
    ON i.run_id=v.run_id AND i.signal_id=v.signal_id
    AND i.decision_date=v.decision_date AND i.security_id=v.security_id
    AND i.term_ordinal=0
  CROSS JOIN pin p CROSS JOIN selected_session s
  WHERE v.run_id=p.run_id AND v.decision_date=s.decision_date
    AND v.signal_id IN ('eps_growth_yoy','operating_margin_change_yoy',
      'low_total_accruals','low_net_debt_ebitda')
), market_ranked AS (
  -- Rank the whole state before any NULL/value screening.
  SELECT m.*,row_number() OVER (PARTITION BY m.security_id
    ORDER BY m.available_at DESC,m.market_daily_id DESC) AS state_rank
  FROM market_daily_metrics m JOIN cohort c ON c.security_id=m.security_id
  CROSS JOIN selected_session s CROSS JOIN coverage x
  WHERE m.source='atx-db daily market panel v1'
    AND m.trade_date=s.decision_date AND m.available_at<=x.decision_at
    AND m.as_of_date<=s.decision_date
), market AS (SELECT * FROM market_ranked WHERE state_rank=1),
joined AS (
  SELECT c.security_id,m.symbol AS market_symbol,m.market_daily_id,
    m.available_at AS market_available_at,m.as_of_date AS market_as_of_date,
    m.source AS market_source,m.run_id AS market_run_id,m.inputs_hash AS market_inputs_hash,
    m.close,m.market_cap,m.earnings_yield,m.cfo_to_ev,
    e.raw_value AS eps_growth_yoy,o.raw_value AS operating_margin_change_yoy,
    a.raw_value AS total_accruals,l.raw_value AS net_debt_ebitda,
    e.eligible AS eps_eligible,o.eligible AS margin_eligible,
    a.eligible AS accruals_eligible,l.eligible AS leverage_eligible,
    e.score_reason AS eps_reason,o.score_reason AS margin_reason,
    a.score_reason AS accruals_reason,l.score_reason AS leverage_reason,
    e.input_reason AS eps_input_reason,o.input_reason AS margin_input_reason,
    a.input_reason AS accruals_input_reason,l.input_reason AS leverage_input_reason,
    e.derived_value_id AS eps_state_id,o.derived_value_id AS margin_state_id,
    a.derived_value_id AS accruals_state_id,l.derived_value_id AS leverage_state_id,
    e.derived_owner_security_id AS eps_owner_id,
    o.derived_owner_security_id AS margin_owner_id,
    a.derived_owner_security_id AS accruals_owner_id,
    l.derived_owner_security_id AS leverage_owner_id,
    e.selected_input_cik AS eps_cik,o.selected_input_cik AS margin_cik,
    a.selected_input_cik AS accruals_cik,l.selected_input_cik AS leverage_cik,
    e.lineage_status AS eps_lineage,o.lineage_status AS margin_lineage,
    a.lineage_status AS accruals_lineage,l.lineage_status AS leverage_lineage,
    e.fiscal_period_end AS eps_fiscal_end,o.fiscal_period_end AS margin_fiscal_end,
    a.fiscal_period_end AS accruals_fiscal_end,l.fiscal_period_end AS leverage_fiscal_end,
    e.available_at AS eps_available_at,o.available_at AS margin_available_at,
    a.available_at AS accruals_available_at,l.available_at AS leverage_available_at
  FROM cohort c LEFT JOIN market m USING (security_id)
  LEFT JOIN legs e ON e.security_id=c.security_id AND e.signal_id='eps_growth_yoy'
  LEFT JOIN legs o ON o.security_id=c.security_id AND o.signal_id='operating_margin_change_yoy'
  LEFT JOIN legs a ON a.security_id=c.security_id AND a.signal_id='low_total_accruals'
  LEFT JOIN legs l ON l.security_id=c.security_id AND l.signal_id='low_net_debt_ebitda'
), classified AS (
  SELECT *,
    market_daily_id IS NOT NULL AND close IS NOT NULL AND isfinite(close) AND close>0 AS valid_close,
    market_daily_id IS NOT NULL AND market_cap IS NOT NULL
      AND isfinite(market_cap) AND market_cap>0 AS valid_market_cap,
    market_daily_id IS NOT NULL AND earnings_yield IS NOT NULL
      AND isfinite(earnings_yield) AS valid_earnings_yield,
    market_daily_id IS NOT NULL AND cfo_to_ev IS NOT NULL
      AND isfinite(cfo_to_ev) AS valid_cfo_to_ev,
    coalesce(eps_eligible,false) AND coalesce(margin_eligible,false)
      AND coalesce(accruals_eligible,false) AND coalesce(leverage_eligible,false)
      AS four_score_eligible,
    eps_input_reason='valid' AND eps_lineage='qualified'
      AND eps_growth_yoy IS NOT NULL AND isfinite(eps_growth_yoy) AS eps_qualified,
    margin_input_reason='valid' AND margin_lineage='qualified'
      AND operating_margin_change_yoy IS NOT NULL AND isfinite(operating_margin_change_yoy)
      AS margin_qualified,
    accruals_input_reason='valid' AND accruals_lineage='qualified'
      AND total_accruals IS NOT NULL AND isfinite(total_accruals)
      AS accruals_qualified,
    leverage_input_reason='valid' AND leverage_lineage='qualified'
      AND net_debt_ebitda IS NOT NULL AND isfinite(net_debt_ebitda)
      AS leverage_qualified
  FROM joined
), disposition AS (
  SELECT *,CASE
    WHEN eps_qualified AND margin_qualified AND accruals_qualified
      AND leverage_qualified AND valid_close AND valid_market_cap
      AND valid_earnings_yield AND valid_cfo_to_ev
      AND eps_growth_yoy>0 AND operating_margin_change_yoy>0 THEN 'passing'
    WHEN eps_qualified AND margin_qualified AND accruals_qualified
      AND leverage_qualified AND valid_close AND valid_market_cap
      AND valid_earnings_yield AND valid_cfo_to_ev THEN 'complete_not_passing'
    ELSE 'incomplete' END AS disposition
  FROM classified
), reasons AS (
  SELECT coalesce(CAST(to_json(list(struct_pack(
    signal_id := signal_id,score_reason := score_reason,
    input_reason := input_reason,rows := reason_rows
  ))) AS VARCHAR),'[]') AS bounded_reasons
  FROM (
    SELECT signal_id,score_reason,input_reason,count(*) AS reason_rows
    FROM legs
    WHERE score_reason<>'valid' OR input_reason<>'valid'
    GROUP BY signal_id,score_reason,input_reason
    ORDER BY reason_rows DESC,signal_id,score_reason,input_reason
    LIMIT 32
  )
), diagnostic AS (
  SELECT 'diagnostic'::VARCHAR AS row_type,
    to_json(struct_pack(
      report_as_of := p.report_as_of,build_run_id := p.run_id,
      decision_date := s.decision_date,decision_cutoff_utc := x.decision_at,
      status := CASE WHEN s.decision_date IS NULL THEN 'no_observed_decision_session'
        WHEN coalesce(x.common_members,0)=0 THEN 'no_historical_common_cohort'
        WHEN count(d.security_id)=0 THEN 'no_cohort_rows'
        WHEN count(*) FILTER (WHERE d.eps_qualified AND d.margin_qualified
          AND d.accruals_qualified AND d.leverage_qualified)=0
          THEN 'no_eligible_historical_common_cohort'
        ELSE 'measured' END,
      coverage_status := x.eps_status,visible_members := coalesce(x.visible_members,0),
      historical_common_members := coalesce(x.common_members,0),
      overlapping_members := coalesce(x.overlap_members,0),
      qualified_cik := coalesce(x.qualified_cik,0),cohort_rows := count(d.security_id),
      bounded_rejection_reasons_json := r.bounded_reasons,
      four_qualified_inputs := count(*) FILTER (WHERE d.eps_qualified
        AND d.margin_qualified AND d.accruals_qualified AND d.leverage_qualified),
      four_score_eligible := count(*) FILTER (WHERE d.four_score_eligible),
      passing := count(*) FILTER (WHERE d.disposition='passing'),
      complete_not_passing := count(*) FILTER (WHERE d.disposition='complete_not_passing'),
      incomplete := count(*) FILTER (WHERE d.disposition='incomplete'),
      missing_market_row := count(*) FILTER (WHERE d.market_daily_id IS NULL AND d.security_id IS NOT NULL),
      invalid_close := count(*) FILTER (WHERE d.security_id IS NOT NULL AND NOT coalesce(d.valid_close,false)),
      invalid_market_cap := count(*) FILTER (WHERE d.security_id IS NOT NULL AND NOT coalesce(d.valid_market_cap,false)),
      invalid_earnings_yield := count(*) FILTER (WHERE d.security_id IS NOT NULL AND NOT coalesce(d.valid_earnings_yield,false)),
      invalid_cfo_to_ev := count(*) FILTER (WHERE d.security_id IS NOT NULL AND NOT coalesce(d.valid_cfo_to_ev,false)),
      unavailable_eps := count(*) FILTER (WHERE d.security_id IS NOT NULL AND NOT coalesce(d.eps_qualified,false)),
      unavailable_margin := count(*) FILTER (WHERE d.security_id IS NOT NULL AND NOT coalesce(d.margin_qualified,false)),
      unavailable_accruals := count(*) FILTER (WHERE d.security_id IS NOT NULL AND NOT coalesce(d.accruals_qualified,false)),
      unavailable_leverage := count(*) FILTER (WHERE d.security_id IS NOT NULL AND NOT coalesce(d.leverage_qualified,false))
    )) AS payload,0 AS sort_group,NULL::DOUBLE AS sort_cap,NULL::VARCHAR AS sort_id
  FROM pin p CROSS JOIN selected_session s CROSS JOIN coverage x
  CROSS JOIN reasons r LEFT JOIN disposition d ON true
  GROUP BY p.report_as_of,p.run_id,s.decision_date,x.decision_at,x.eps_status,
    x.visible_members,x.common_members,x.overlap_members,x.qualified_cik,
    r.bounded_reasons
), preview AS (
  SELECT 'name'::VARCHAR AS row_type,
    to_json(struct_pack(
      security_id := d.security_id,market_symbol := d.market_symbol,
      disposition := d.disposition,
      eps_growth_yoy := CASE WHEN d.eps_qualified THEN d.eps_growth_yoy END,
      operating_margin_change_yoy := CASE WHEN d.margin_qualified
        THEN d.operating_margin_change_yoy END,
      total_accruals := CASE WHEN d.accruals_qualified THEN d.total_accruals END,
      net_debt_ebitda := CASE WHEN d.leverage_qualified THEN d.net_debt_ebitda END,
      close := CASE WHEN isfinite(d.close) THEN d.close END,
      market_cap := CASE WHEN isfinite(d.market_cap) THEN d.market_cap END,
      earnings_yield := CASE WHEN isfinite(d.earnings_yield) THEN d.earnings_yield END,
      cfo_to_ev := CASE WHEN isfinite(d.cfo_to_ev) THEN d.cfo_to_ev END,
      market_daily_id := d.market_daily_id,
      market_available_at := d.market_available_at,
      market_as_of_date := d.market_as_of_date,
      market_source := d.market_source,market_run_id := d.market_run_id,
      market_inputs_hash := d.market_inputs_hash,
      valid_close := d.valid_close,valid_market_cap := d.valid_market_cap,
      valid_earnings_yield := d.valid_earnings_yield,
      valid_cfo_to_ev := d.valid_cfo_to_ev,
      eps_eligible := d.eps_eligible,margin_eligible := d.margin_eligible,
      accruals_eligible := d.accruals_eligible,leverage_eligible := d.leverage_eligible,
      eps_qualified := d.eps_qualified,margin_qualified := d.margin_qualified,
      accruals_qualified := d.accruals_qualified,leverage_qualified := d.leverage_qualified,
      eps_reason := d.eps_reason,margin_reason := d.margin_reason,
      accruals_reason := d.accruals_reason,leverage_reason := d.leverage_reason,
      eps_input_reason := d.eps_input_reason,margin_input_reason := d.margin_input_reason,
      accruals_input_reason := d.accruals_input_reason,
      leverage_input_reason := d.leverage_input_reason,
      eps_state_id := d.eps_state_id,margin_state_id := d.margin_state_id,
      accruals_state_id := d.accruals_state_id,leverage_state_id := d.leverage_state_id,
      eps_owner_id := d.eps_owner_id,margin_owner_id := d.margin_owner_id,
      accruals_owner_id := d.accruals_owner_id,leverage_owner_id := d.leverage_owner_id,
      eps_cik := d.eps_cik,margin_cik := d.margin_cik,
      accruals_cik := d.accruals_cik,leverage_cik := d.leverage_cik,
      eps_lineage := d.eps_lineage,margin_lineage := d.margin_lineage,
      accruals_lineage := d.accruals_lineage,leverage_lineage := d.leverage_lineage,
      eps_fiscal_end := d.eps_fiscal_end,margin_fiscal_end := d.margin_fiscal_end,
      accruals_fiscal_end := d.accruals_fiscal_end,leverage_fiscal_end := d.leverage_fiscal_end,
      eps_available_at := d.eps_available_at,margin_available_at := d.margin_available_at,
      accruals_available_at := d.accruals_available_at,
      leverage_available_at := d.leverage_available_at
    )) AS payload,
    CASE d.disposition WHEN 'passing' THEN 1
      WHEN 'complete_not_passing' THEN 2 ELSE 3 END AS sort_group,
    d.market_cap AS sort_cap,d.security_id AS sort_id
  FROM disposition d
  ORDER BY sort_group,sort_cap DESC NULLS LAST,sort_id LIMIT 999
)
SELECT row_type,payload FROM (
  SELECT * FROM diagnostic UNION ALL SELECT * FROM preview
) ORDER BY sort_group,sort_cap DESC NULLS LAST,sort_id;
