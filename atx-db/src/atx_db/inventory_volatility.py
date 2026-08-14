"""Point-in-time within-year quarterly inventory volatility factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .connection import DuckDBStore
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT within-year inventory volatility v1"
FACTOR_ID = "operations_high_inventory_volatility"
FACTOR_NAME = "PIT high within-year inventory volatility"


@dataclass(frozen=True)
class InventoryVolatilityOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_reporting_age_days: int = 200
    maximum_history_days: int = 500
    minimum_four_quarter_span_days: int = 240
    maximum_four_quarter_span_days: int = 310
    maximum_raw_value: float = 5.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_inventory_volatility_values(
    store: DuckDBStore,
    options: InventoryVolatilityOptions | None = None,
) -> int:
    """Materialize the coefficient of variation of four visible inventory quarters."""

    options = options or InventoryVolatilityOptions()
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
    input_ids = json_dumps([f"factor:{ASSET_GROWTH_FACTOR_ID}", "metric:inventory"])
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
        visible_inventory AS (
            SELECT p.*,q.period_end,q.value AS inventory_value,
                   q.statement_point_id AS inventory_id,
                   q.accession_number,q.available_at AS inventory_available_at,
                   row_number() OVER (
                       PARTITION BY p.security_id,p.as_of_date,q.period_end
                       ORDER BY q.available_at DESC,q.revision_sequence DESC,
                                q.statement_point_id DESC
                   ) AS point_rank
            FROM parent p
            JOIN fundamental_statement_points q ON q.security_id=p.security_id
             AND q.canonical_metric='inventory' AND q.unit='USD'
             AND q.period_type='instant' AND q.period_end<=p.as_of_date
             AND p.as_of_date-q.period_end<=?
             AND q.available_at<=p.decision_available_at
             AND q.form IN ('10-Q','10-Q/A','10-QT','10-K','10-K/A','10-KT',
                            '20-F','20-F/A','40-F','40-F/A','6-K','6-K/A')
             AND q.value>0 AND isfinite(q.value)
        ),
        as_of_points AS (
            SELECT * EXCLUDE (point_rank),
                   row_number() OVER (
                       PARTITION BY security_id,as_of_date ORDER BY period_end DESC
                   ) AS quarter_rank
            FROM visible_inventory WHERE point_rank=1
        ),
        summarized AS (
            SELECT parent_factor_value_id,security_id,symbol,as_of_date,
                   decision_available_at,count(*) AS observation_count,
                   min(period_end) AS first_period_end,max(period_end) AS last_period_end,
                   avg(inventory_value) AS mean_inventory,
                   stddev_samp(inventory_value) AS inventory_standard_deviation,
                   list(period_end ORDER BY period_end) AS inventory_period_ends,
                   list(inventory_value ORDER BY period_end) AS inventory_values,
                   list(inventory_id ORDER BY period_end) AS inventory_ids,
                   list(accession_number ORDER BY period_end) AS inventory_accessions,
                   list(inventory_available_at ORDER BY period_end)
                       AS inventory_available_ats
            FROM as_of_points WHERE quarter_rank<=4
            GROUP BY parent_factor_value_id,security_id,symbol,as_of_date,
                     decision_available_at
            HAVING count(*)=4
        ),
        raw AS (
            SELECT *,inventory_standard_deviation/mean_inventory AS raw_value
            FROM summarized
            WHERE as_of_date-last_period_end<=?
              AND last_period_end-first_period_end BETWEEN ? AND ?
              AND mean_inventory>0
              AND inventory_standard_deviation/mean_inventory<=?
              AND isfinite(inventory_standard_deviation/mean_inventory)
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY as_of_date) AS names_on_date FROM raw
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
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_operations',security_id,symbol,
            as_of_date,raw_value,(winsorized_value-cs_mean)/cs_std,decision_available_at,?,
            cast(to_json(struct_pack(
                method := 'four_quarter_inventory_coefficient_of_variation_pit',
                formula := 'stddev_samp(inventory_q0..q3)/mean(inventory_q0..q3)',
                orientation := 'higher_inventory_volatility_is_preferred',
                observation_count := observation_count,
                maximum_reporting_age_days := ?,maximum_history_days := ?,
                minimum_four_quarter_span_days := ?,maximum_four_quarter_span_days := ?,
                maximum_raw_value := ?,positive_inventory_required := true,
                missing_components_imputed := false,return_fitted_parameters := false,
                parent_factor_id := '{ASSET_GROWTH_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                first_period_end := first_period_end,last_period_end := last_period_end,
                mean_inventory := mean_inventory,
                inventory_standard_deviation := inventory_standard_deviation,
                inventory_period_ends := inventory_period_ends,
                inventory_values := inventory_values,inventory_ids := inventory_ids,
                inventory_accessions := inventory_accessions,
                inventory_available_ats := inventory_available_ats,
                decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        *parent_params,
        options.maximum_history_days,
        options.maximum_reporting_age_days,
        options.minimum_four_quarter_span_days,
        options.maximum_four_quarter_span_days,
        options.maximum_raw_value,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids,
        options.maximum_reporting_age_days,
        options.maximum_history_days,
        options.minimum_four_quarter_span_days,
        options.maximum_four_quarter_span_days,
        options.maximum_raw_value,
        options.run_id,
        options.source,
    ]
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
            delete_params,
        )
        store.con.execute(sql,params)
    row = store.con.execute(
        f"SELECT count(*) FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
        delete_params,
    ).fetchone()
    return int(row[0])
