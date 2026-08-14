"""Point-in-time Beneish eight-variable earnings-manipulation score factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .connection import DuckDBStore
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT low Beneish manipulation risk v1"
FACTOR_ID = "quality_low_beneish_m_score"
FACTOR_NAME = "PIT low Beneish manipulation risk"


@dataclass(frozen=True)
class BeneishMScoreOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_annual_gap_days: int = 300
    maximum_annual_gap_days: int = 430
    maximum_reporting_age_days: int = 550
    maximum_history_days: int = 1000
    maximum_absolute_m_score: float = 100.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_beneish_m_score_values(
    store: DuckDBStore,
    options: BeneishMScoreOptions | None = None,
) -> int:
    """Materialize the negative Beneish M-score so lower manipulation risk ranks high."""

    options = options or BeneishMScoreOptions()
    store.initialize()
    parent_predicates = ["factor_id=?", "is_latest_revision"]
    parent_params: list[object] = [ASSET_GROWTH_FACTOR_ID]
    delete_predicates = ["factor_id=?", "source=?"]
    delete_params: list[object] = [FACTOR_ID, options.source]
    if options.start_date is not None:
        parent_predicates.append("as_of_date>=?")
        parent_params.append(options.start_date)
        delete_predicates.append("as_of_date>=?")
        delete_params.append(options.start_date)
    if options.end_date is not None:
        parent_predicates.append("as_of_date<=?")
        parent_params.append(options.end_date)
        delete_predicates.append("as_of_date<=?")
        delete_params.append(options.end_date)
    metrics = (
        "revenue",
        "ar",
        "cogs",
        "total_assets",
        "current_assets",
        "ppe_net",
        "da_cf",
        "da_is",
        "depreciation",
        "sga",
        "total_liabilities",
        "net_income",
        "operating_cash_flow",
    )
    input_ids = json_dumps(
        [f"factor:{ASSET_GROWTH_FACTOR_ID}", *(f"metric:{metric}" for metric in metrics)]
    )
    sql = f"""
        INSERT INTO fundamental_factor_values (
            factor_value_id,factor_id,factor_name,family,security_id,symbol,
            as_of_date,raw_value,value,available_at,input_ids_json,
            input_lineage_json,is_latest_revision,run_id,source
        )
        WITH parent AS (
            SELECT factor_value_id AS parent_factor_value_id,security_id,symbol,
                   as_of_date,available_at AS decision_available_at
            FROM fundamental_factor_values
            WHERE {' AND '.join(parent_predicates)}
            QUALIFY row_number() OVER (
                PARTITION BY security_id,as_of_date
                ORDER BY available_at DESC,factor_value_id DESC
            )=1
        ),
        annual_raw AS (
            SELECT security_id,accession_number,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='revenue') AS revenue,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='revenue') AS revenue_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='ar') AS receivables,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='ar') AS receivables_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='cogs') AS cogs,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='cogs') AS cogs_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='total_assets') AS total_assets,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='total_assets') AS total_assets_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='current_assets') AS current_assets,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='current_assets') AS current_assets_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='ppe_net') AS ppe_net,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='ppe_net') AS ppe_net_id,
                   coalesce(
                       arg_max(value,(available_at,revision_sequence,statement_point_id))
                           FILTER (WHERE canonical_metric='da_cf'),
                       arg_max(value,(available_at,revision_sequence,statement_point_id))
                           FILTER (WHERE canonical_metric='da_is'),
                       arg_max(value,(available_at,revision_sequence,statement_point_id))
                           FILTER (WHERE canonical_metric='depreciation')
                   ) AS depreciation_amortization,
                   coalesce(
                       arg_max(statement_point_id,
                               (available_at,revision_sequence,statement_point_id))
                           FILTER (WHERE canonical_metric='da_cf'),
                       arg_max(statement_point_id,
                               (available_at,revision_sequence,statement_point_id))
                           FILTER (WHERE canonical_metric='da_is'),
                       arg_max(statement_point_id,
                               (available_at,revision_sequence,statement_point_id))
                           FILTER (WHERE canonical_metric='depreciation')
                   ) AS depreciation_amortization_id,
                   coalesce(
                       arg_max(available_at,
                               (available_at,revision_sequence,statement_point_id))
                           FILTER (WHERE canonical_metric='da_cf'),
                       arg_max(available_at,
                               (available_at,revision_sequence,statement_point_id))
                           FILTER (WHERE canonical_metric='da_is'),
                       arg_max(available_at,
                               (available_at,revision_sequence,statement_point_id))
                           FILTER (WHERE canonical_metric='depreciation')
                   ) AS depreciation_amortization_available_at,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='sga') AS sga,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='sga') AS sga_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='total_liabilities')
                       AS total_liabilities,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='total_liabilities')
                       AS total_liabilities_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='net_income') AS net_income,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='net_income') AS net_income_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='operating_cash_flow')
                       AS operating_cash_flow,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='operating_cash_flow')
                       AS operating_cash_flow_id,
                   max(available_at) AS annual_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric IN (
                    'revenue','ar','cogs','total_assets','current_assets','ppe_net',
                    'da_cf','da_is','depreciation','sga','total_liabilities',
                    'net_income','operating_cash_flow'
                  )
              AND unit='USD' AND accession_number IS NOT NULL AND period_end IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
              AND (
                    period_type='instant'
                    OR (period_type='duration' AND period_start IS NOT NULL
                        AND period_end-period_start BETWEEN 329 AND 399)
                  )
            GROUP BY security_id,accession_number,period_end
        ),
        annual AS (
            SELECT * FROM annual_raw
            WHERE revenue>0 AND receivables>=0 AND cogs>=0 AND total_assets>0
              AND current_assets>=0 AND ppe_net>=0 AND depreciation_amortization>0
              AND sga>=0 AND total_liabilities>0
              AND net_income IS NOT NULL AND operating_cash_flow IS NOT NULL
              AND isfinite(revenue) AND isfinite(receivables) AND isfinite(cogs)
              AND isfinite(total_assets) AND isfinite(current_assets)
              AND isfinite(ppe_net) AND isfinite(depreciation_amortization)
              AND isfinite(sga) AND isfinite(total_liabilities)
              AND isfinite(net_income) AND isfinite(operating_cash_flow)
        ),
        visible AS (
            SELECT p.*,a.* EXCLUDE (security_id),
                   row_number() OVER (
                       PARTITION BY p.security_id,p.as_of_date,a.period_end
                       ORDER BY a.annual_available_at DESC,a.accession_number DESC
                   ) AS amendment_rank
            FROM parent p JOIN annual a ON a.security_id=p.security_id
             AND a.period_end<=p.as_of_date AND p.as_of_date-a.period_end<=?
             AND a.annual_available_at<=p.decision_available_at
        ),
        ranked AS (
            SELECT * EXCLUDE (amendment_rank),
                   row_number() OVER (
                       PARTITION BY security_id,as_of_date ORDER BY period_end DESC
                   ) AS annual_rank
            FROM visible WHERE amendment_rank=1
        ),
        summarized AS (
            SELECT parent_factor_value_id,security_id,symbol,as_of_date,
                   decision_available_at,count(*) AS annual_observations,
                   max(period_end) FILTER (WHERE annual_rank=1) AS current_period_end,
                   max(period_end) FILTER (WHERE annual_rank=2) AS prior_period_end,
                   max(revenue) FILTER (WHERE annual_rank=1) AS revenue,
                   max(revenue) FILTER (WHERE annual_rank=2) AS prior_revenue,
                   max(receivables) FILTER (WHERE annual_rank=1) AS receivables,
                   max(receivables) FILTER (WHERE annual_rank=2) AS prior_receivables,
                   max(cogs) FILTER (WHERE annual_rank=1) AS cogs,
                   max(cogs) FILTER (WHERE annual_rank=2) AS prior_cogs,
                   max(total_assets) FILTER (WHERE annual_rank=1) AS total_assets,
                   max(total_assets) FILTER (WHERE annual_rank=2) AS prior_total_assets,
                   max(current_assets) FILTER (WHERE annual_rank=1) AS current_assets,
                   max(current_assets) FILTER (WHERE annual_rank=2) AS prior_current_assets,
                   max(ppe_net) FILTER (WHERE annual_rank=1) AS ppe_net,
                   max(ppe_net) FILTER (WHERE annual_rank=2) AS prior_ppe_net,
                   max(depreciation_amortization) FILTER (WHERE annual_rank=1)
                       AS depreciation_amortization,
                   max(depreciation_amortization) FILTER (WHERE annual_rank=2)
                       AS prior_depreciation_amortization,
                   max(sga) FILTER (WHERE annual_rank=1) AS sga,
                   max(sga) FILTER (WHERE annual_rank=2) AS prior_sga,
                   max(total_liabilities) FILTER (WHERE annual_rank=1)
                       AS total_liabilities,
                   max(total_liabilities) FILTER (WHERE annual_rank=2)
                       AS prior_total_liabilities,
                   max(net_income) FILTER (WHERE annual_rank=1) AS net_income,
                   max(operating_cash_flow) FILTER (WHERE annual_rank=1)
                       AS operating_cash_flow,
                   list(accession_number ORDER BY period_end DESC) AS accessions,
                   list(annual_available_at ORDER BY period_end DESC)
                       AS annual_available_ats,
                   list(revenue_id ORDER BY period_end DESC) AS revenue_ids,
                   list(receivables_id ORDER BY period_end DESC) AS receivables_ids,
                   list(cogs_id ORDER BY period_end DESC) AS cogs_ids,
                   list(total_assets_id ORDER BY period_end DESC) AS total_assets_ids,
                   list(current_assets_id ORDER BY period_end DESC) AS current_assets_ids,
                   list(ppe_net_id ORDER BY period_end DESC) AS ppe_net_ids,
                   list(depreciation_amortization_id ORDER BY period_end DESC)
                       AS depreciation_amortization_ids,
                   list(sga_id ORDER BY period_end DESC) AS sga_ids,
                   list(total_liabilities_id ORDER BY period_end DESC)
                       AS total_liabilities_ids,
                   list(net_income_id ORDER BY period_end DESC) AS net_income_ids,
                   list(operating_cash_flow_id ORDER BY period_end DESC)
                       AS operating_cash_flow_ids
            FROM ranked WHERE annual_rank<=2
            GROUP BY parent_factor_value_id,security_id,symbol,as_of_date,
                     decision_available_at
            HAVING count(*)=2
        ),
        base AS (
            SELECT *,
                   (revenue-cogs)/revenue AS gross_margin,
                   (prior_revenue-prior_cogs)/prior_revenue AS prior_gross_margin,
                   1-(current_assets+ppe_net)/total_assets AS asset_quality,
                   1-(prior_current_assets+prior_ppe_net)/prior_total_assets
                       AS prior_asset_quality,
                   depreciation_amortization/(depreciation_amortization+ppe_net)
                       AS depreciation_rate,
                   prior_depreciation_amortization
                       /(prior_depreciation_amortization+prior_ppe_net)
                       AS prior_depreciation_rate
            FROM summarized
            WHERE as_of_date-current_period_end<=?
              AND current_period_end-prior_period_end BETWEEN ? AND ?
              AND prior_receivables>0 AND prior_sga>0
        ),
        indices AS (
            SELECT *,
                   (receivables/revenue)/(prior_receivables/prior_revenue) AS dsri,
                   prior_gross_margin/gross_margin AS gmi,
                   asset_quality/prior_asset_quality AS aqi,
                   revenue/prior_revenue AS sgi,
                   prior_depreciation_rate/depreciation_rate AS depi,
                   (sga/revenue)/(prior_sga/prior_revenue) AS sgai,
                   (net_income-operating_cash_flow)/total_assets AS tata,
                   (total_liabilities/total_assets)
                       /(prior_total_liabilities/prior_total_assets) AS lvgi
            FROM base
            WHERE gross_margin>0 AND prior_gross_margin>0
              AND prior_asset_quality<>0 AND depreciation_rate>0
        ),
        scored AS (
            SELECT *,
                   -4.84+0.920*dsri+0.528*gmi+0.404*aqi+0.892*sgi
                       +0.115*depi-0.172*sgai+4.679*tata-0.327*lvgi AS m_score
            FROM indices
        ),
        eligible AS (
            SELECT *,-m_score AS raw_value FROM scored
            WHERE abs(m_score)<=? AND isfinite(m_score)
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY as_of_date) AS names_on_date
            FROM eligible
        ),
        bounded AS (
            SELECT *,quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date)
                       AS lower_bound,
                   quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date)
                       AS upper_bound
            FROM breadth WHERE names_on_date>=?
        ),
        moments AS (
            SELECT *,greatest(lower_bound,least(upper_bound,raw_value))
                       AS winsorized_value
            FROM bounded
        ),
        standardized AS (
            SELECT *,avg(winsorized_value) OVER (PARTITION BY as_of_date) AS cs_mean,
                   stddev_samp(winsorized_value) OVER (PARTITION BY as_of_date) AS cs_std
            FROM moments
        )
        SELECT
            sha256(concat_ws('|',?,'{FACTOR_ID}',security_id,cast(as_of_date AS VARCHAR))),
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_quality',security_id,symbol,
            as_of_date,raw_value,(winsorized_value-cs_mean)/cs_std,
            decision_available_at,?,
            cast(to_json(struct_pack(
                method := 'beneish_1999_eight_variable_m_score_pit',
                formula := '-(-4.84+0.920*DSRI+0.528*GMI+0.404*AQI+0.892*SGI+0.115*DEPI-0.172*SGAI+4.679*TATA-0.327*LVGI)',
                orientation := 'lower_manipulation_probability_is_higher_quality',
                m_score := m_score,classic_manipulator_threshold := -2.22,
                dsri := dsri,gmi := gmi,aqi := aqi,sgi := sgi,depi := depi,
                sgai := sgai,tata := tata,lvgi := lvgi,
                maximum_reporting_age_days := ?,maximum_history_days := ?,
                annual_gap_days := list_value(?,?),maximum_absolute_m_score := ?,
                missing_components_imputed := false,return_fitted_parameters := false,
                depreciation_precedence := list_value('da_cf','da_is','depreciation'),
                parent_factor_id := '{ASSET_GROWTH_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                current_period_end := current_period_end,prior_period_end := prior_period_end,
                accessions := accessions,annual_available_ats := annual_available_ats,
                revenue_values := list_value(revenue,prior_revenue),
                receivables_values := list_value(receivables,prior_receivables),
                cogs_values := list_value(cogs,prior_cogs),
                total_assets_values := list_value(total_assets,prior_total_assets),
                current_assets_values := list_value(current_assets,prior_current_assets),
                ppe_net_values := list_value(ppe_net,prior_ppe_net),
                depreciation_amortization_values :=
                    list_value(depreciation_amortization,prior_depreciation_amortization),
                sga_values := list_value(sga,prior_sga),
                total_liabilities_values :=
                    list_value(total_liabilities,prior_total_liabilities),
                net_income := net_income,operating_cash_flow := operating_cash_flow,
                revenue_ids := revenue_ids,receivables_ids := receivables_ids,
                cogs_ids := cogs_ids,total_assets_ids := total_assets_ids,
                current_assets_ids := current_assets_ids,ppe_net_ids := ppe_net_ids,
                depreciation_amortization_ids := depreciation_amortization_ids,
                sga_ids := sga_ids,total_liabilities_ids := total_liabilities_ids,
                net_income_ids := net_income_ids,
                operating_cash_flow_ids := operating_cash_flow_ids,
                decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        *parent_params,
        options.maximum_history_days,
        options.maximum_reporting_age_days,
        options.minimum_annual_gap_days,
        options.maximum_annual_gap_days,
        options.maximum_absolute_m_score,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids,
        options.maximum_reporting_age_days,
        options.maximum_history_days,
        options.minimum_annual_gap_days,
        options.maximum_annual_gap_days,
        options.maximum_absolute_m_score,
        options.run_id,
        options.source,
    ]
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
            delete_params,
        )
        store.con.execute(sql, params)
    row = store.con.execute(
        f"SELECT count(*) FROM fundamental_factor_values "
        f"WHERE {' AND '.join(delete_predicates)}",
        delete_params,
    ).fetchone()
    return int(row[0])
