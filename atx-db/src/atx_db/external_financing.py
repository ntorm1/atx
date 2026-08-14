"""Point-in-time comprehensive net external-financing factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .connection import DuckDBStore
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT net external financing v1"
FACTOR_ID = "financing_low_external_financing"
FACTOR_NAME = "PIT low net external financing"


@dataclass(frozen=True)
class ExternalFinancingOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_external_financing_values(
    store: DuckDBStore,
    options: ExternalFinancingOptions | None = None,
) -> int:
    """Materialize negative annual financing cash flow over beginning assets."""

    options = options or ExternalFinancingOptions()
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
    input_ids = json_dumps(
        [f"factor:{ASSET_GROWTH_FACTOR_ID}", "metric:financing_cash_flow"]
    )
    sql = f"""
        INSERT INTO fundamental_factor_values (
            factor_value_id,factor_id,factor_name,family,security_id,symbol,
            as_of_date,raw_value,value,available_at,input_ids_json,
            input_lineage_json,is_latest_revision,run_id,source
        )
        WITH parent AS (
            SELECT
                factor_value_id AS parent_factor_value_id,security_id,symbol,
                as_of_date,available_at AS decision_available_at,
                json_extract_string(input_lineage_json,'$.assets.current.accession_number')
                    AS accession_number,
                CAST(json_extract_string(input_lineage_json,'$.assets.current.period_end') AS DATE)
                    AS annual_period_end,
                json_extract_string(input_lineage_json,'$.assets.prior.statement_point_id')
                    AS prior_asset_id,
                CAST(json_extract_string(input_lineage_json,'$.assets.prior.period_end') AS DATE)
                    AS prior_asset_period_end,
                try_cast(json_extract_string(input_lineage_json,'$.assets.prior.value') AS DOUBLE)
                    AS prior_assets,
                CAST(json_extract_string(input_lineage_json,'$.assets.prior.available_at') AS TIMESTAMP)
                    AS prior_asset_available_at
            FROM fundamental_factor_values
            WHERE {' AND '.join(parent_predicates)}
            QUALIFY row_number() OVER (
                PARTITION BY security_id,as_of_date
                ORDER BY available_at DESC,factor_value_id DESC
            )=1
        ),
        financing AS (
            SELECT security_id,accession_number,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       AS financing_cash_flow,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       AS financing_cash_flow_id,
                   max(available_at) AS financing_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric='financing_cash_flow' AND unit='USD'
              AND period_type='duration' AND period_start IS NOT NULL
              AND period_end-period_start BETWEEN 329 AND 399
              AND accession_number IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
              AND isfinite(value)
            GROUP BY security_id,accession_number,period_end
        ),
        raw AS (
            SELECT p.*,f.financing_cash_flow,f.financing_cash_flow_id,
                   f.financing_available_at,-f.financing_cash_flow/p.prior_assets AS raw_value
            FROM parent p
            JOIN financing f ON f.security_id=p.security_id
             AND f.accession_number=p.accession_number
             AND f.period_end=p.annual_period_end
             AND f.financing_available_at<=p.decision_available_at
            WHERE p.prior_assets>0 AND isfinite(p.prior_assets)
              AND isfinite(-f.financing_cash_flow/p.prior_assets)
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY as_of_date) AS names_on_date
            FROM raw
        ),
        bounded AS (
            SELECT *,quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date) AS lower_bound,
                   quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date) AS upper_bound
            FROM breadth WHERE names_on_date>=?
        ),
        moments AS (
            SELECT *,greatest(lower_bound,least(upper_bound,raw_value)) AS winsorized_value
            FROM bounded
        ),
        standardized AS (
            SELECT *,avg(winsorized_value) OVER (PARTITION BY as_of_date) AS cs_mean,
                   stddev_samp(winsorized_value) OVER (PARTITION BY as_of_date) AS cs_std
            FROM moments
        )
        SELECT
            sha256(concat_ws('|',?,'{FACTOR_ID}',security_id,cast(as_of_date AS VARCHAR))),
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_financing',security_id,symbol,
            as_of_date,raw_value,(winsorized_value-cs_mean)/cs_std,decision_available_at,?,
            cast(to_json(struct_pack(
                method := 'bradshaw_richardson_sloan_external_financing_pit',
                formula := '-financing_cash_flow/prior_total_assets',
                orientation := 'lower_net_external_financing_is_preferred',
                missing_components_imputed := false,
                return_fitted_parameters := false,
                parent_factor_id := '{ASSET_GROWTH_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                accession_number := accession_number,
                annual_period_end := annual_period_end,
                financing_cash_flow := financing_cash_flow,
                financing_cash_flow_id := financing_cash_flow_id,
                financing_available_at := financing_available_at,
                prior_assets := prior_assets,
                prior_asset_id := prior_asset_id,
                prior_asset_period_end := prior_asset_period_end,
                prior_asset_available_at := prior_asset_available_at,
                decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        *parent_params,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids,
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
        f"SELECT count(*) FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
        delete_params,
    ).fetchone()
    return int(row[0])
