"""Point-in-time low abnormal capital-investment factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .connection import DuckDBStore
from .universe import DEFAULT_UNIVERSE_ID
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT abnormal capital investment v1"
FACTOR_ID = "investment_low_abnormal_capex"
FACTOR_NAME = "PIT low abnormal capital investment"


@dataclass(frozen=True)
class AbnormalCapexOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_current_age_days: int = 200
    lag_tolerance_days: int = 100
    maximum_absolute_abnormal_ratio: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_abnormal_capex_values(
    store: DuckDBStore,
    options: AbnormalCapexOptions | None = None,
) -> int:
    """Materialize low current capex relative to the prior three annual TTM values."""

    options = options or AbnormalCapexOptions()
    store.initialize()
    predicates: list[str] = []
    date_params: list[object] = []
    delete_predicates = ["factor_id = ?", "source = ?"]
    delete_params: list[object] = [FACTOR_ID, options.source]
    if options.start_date is not None:
        predicates.append("m.trade_date >= ?")
        date_params.append(options.start_date)
        delete_predicates.append("as_of_date >= ?")
        delete_params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("m.trade_date <= ?")
        date_params.append(options.end_date)
        delete_predicates.append("as_of_date <= ?")
        delete_params.append(options.end_date)
    date_sql = " AND " + " AND ".join(predicates) if predicates else ""
    input_ids_json = json_dumps(
        [
            "dataset:market_cap",
            "metric:capital_expenditures",
            f"universe:{options.universe_id}",
        ]
    )
    sql = f"""
        INSERT INTO fundamental_factor_values (
            factor_value_id,factor_id,factor_name,family,security_id,symbol,
            as_of_date,raw_value,value,available_at,input_ids_json,
            input_lineage_json,is_latest_revision,run_id,source
        )
        WITH cap_months AS (
            SELECT
                m.*,
                row_number() OVER (
                    PARTITION BY m.security_id,year(m.trade_date),month(m.trade_date)
                    ORDER BY m.trade_date DESC,m.available_at DESC,m.market_cap_id DESC
                ) AS month_rank
            FROM market_cap m
            WHERE m.is_latest_revision
              AND m.market_cap > 0
              AND isfinite(m.market_cap)
              {date_sql}
        ),
        rebalances AS (
            SELECT * EXCLUDE (month_rank)
            FROM cap_months
            WHERE month_rank = 1
        ),
        governed AS (
            SELECT
                m.*,
                u.valid_from AS universe_valid_from,
                u.valid_to AS universe_valid_to,
                u.available_at AS universe_available_at,
                u.source AS universe_source,
                row_number() OVER (
                    PARTITION BY m.security_id,m.trade_date
                    ORDER BY u.valid_from DESC,u.available_at DESC NULLS LAST,
                             u.source_loaded_at DESC,u.source DESC
                ) AS universe_rank
            FROM rebalances m
            JOIN universe_membership u
              ON u.universe_id = ?
             AND u.security_id = m.security_id
             AND u.valid_from <= m.trade_date
             AND (u.valid_to IS NULL OR u.valid_to >= m.trade_date)
             AND u.as_of_date <= m.trade_date
             AND u.is_member
             AND u.is_latest_revision
             AND (u.available_at IS NULL OR u.available_at <= m.available_at)
        ),
        current_candidates AS (
            SELECT
                d.security_id,
                d.symbol,
                d.trade_date,
                d.market_cap_id,
                d.market_cap,
                d.available_at AS decision_available_at,
                d.universe_valid_from,
                d.universe_valid_to,
                d.universe_available_at,
                d.universe_source,
                c.ttm_end_date AS current_period_end,
                -c.ttm_value AS current_capex,
                c.ttm_point_id AS current_ttm_point_id,
                c.available_at AS current_capex_available_at,
                c.source AS current_capex_source,
                c.input_statement_point_ids_json AS current_input_statement_point_ids_json,
                c.input_accessions_json AS current_input_accessions_json,
                row_number() OVER (
                    PARTITION BY d.security_id,d.trade_date
                    ORDER BY c.ttm_end_date DESC,c.available_at DESC,
                             c.revision_sequence DESC,c.ttm_point_id DESC
                ) AS current_rank
            FROM governed d
            JOIN fundamental_ttm_points c
              ON c.security_id = d.security_id
             AND c.canonical_metric = 'capital_expenditures'
             AND c.unit = 'USD'
             AND c.ttm_value < 0
             AND isfinite(c.ttm_value)
             AND c.ttm_end_date <= d.trade_date
             AND c.available_at <= d.available_at
             AND d.trade_date - c.ttm_end_date <= ?
            WHERE d.universe_rank = 1
        ),
        current_rows AS (
            SELECT * EXCLUDE (current_rank)
            FROM current_candidates
            WHERE current_rank = 1
        ),
        prior_candidates AS (
            SELECT
                c.*,
                round((c.current_period_end-p.ttm_end_date)/365.25)::INTEGER AS lag_year,
                -p.ttm_value AS prior_capex,
                p.ttm_point_id AS prior_ttm_point_id,
                p.ttm_end_date AS prior_period_end,
                p.available_at AS prior_available_at,
                p.source AS prior_source,
                row_number() OVER (
                    PARTITION BY c.security_id,c.trade_date,
                        round((c.current_period_end-p.ttm_end_date)/365.25)::INTEGER
                    ORDER BY
                        abs(
                            (c.current_period_end-p.ttm_end_date)
                            - round((c.current_period_end-p.ttm_end_date)/365.25)*365.25
                        ),
                        p.ttm_end_date DESC,p.available_at DESC,
                        p.revision_sequence DESC,p.ttm_point_id DESC
                ) AS lag_rank
            FROM current_rows c
            JOIN fundamental_ttm_points p
              ON p.security_id = c.security_id
             AND p.canonical_metric = 'capital_expenditures'
             AND p.unit = 'USD'
             AND p.ttm_value < 0
             AND isfinite(p.ttm_value)
             AND p.ttm_end_date < c.current_period_end
             AND p.available_at <= c.decision_available_at
            WHERE round((c.current_period_end-p.ttm_end_date)/365.25)::INTEGER BETWEEN 1 AND 3
              AND abs(
                    (c.current_period_end-p.ttm_end_date)
                    - round((c.current_period_end-p.ttm_end_date)/365.25)*365.25
                  ) <= ?
        ),
        baseline AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                any_value(decision_available_at) AS decision_available_at,
                any_value(market_cap_id) AS market_cap_id,
                any_value(market_cap) AS market_cap,
                any_value(universe_valid_from) AS universe_valid_from,
                any_value(universe_valid_to) AS universe_valid_to,
                any_value(universe_available_at) AS universe_available_at,
                any_value(universe_source) AS universe_source,
                any_value(current_period_end) AS current_period_end,
                any_value(current_capex) AS current_capex,
                any_value(current_ttm_point_id) AS current_ttm_point_id,
                any_value(current_capex_available_at) AS current_capex_available_at,
                any_value(current_capex_source) AS current_capex_source,
                any_value(current_input_statement_point_ids_json)
                    AS current_input_statement_point_ids_json,
                any_value(current_input_accessions_json) AS current_input_accessions_json,
                avg(prior_capex) AS baseline_capex,
                max(prior_capex) FILTER (WHERE lag_year=1) AS prior_1y_capex,
                max(prior_capex) FILTER (WHERE lag_year=2) AS prior_2y_capex,
                max(prior_capex) FILTER (WHERE lag_year=3) AS prior_3y_capex,
                max(prior_ttm_point_id) FILTER (WHERE lag_year=1) AS prior_1y_id,
                max(prior_ttm_point_id) FILTER (WHERE lag_year=2) AS prior_2y_id,
                max(prior_ttm_point_id) FILTER (WHERE lag_year=3) AS prior_3y_id,
                max(prior_period_end) FILTER (WHERE lag_year=1) AS prior_1y_period_end,
                max(prior_period_end) FILTER (WHERE lag_year=2) AS prior_2y_period_end,
                max(prior_period_end) FILTER (WHERE lag_year=3) AS prior_3y_period_end,
                max(prior_available_at) FILTER (WHERE lag_year=1) AS prior_1y_available_at,
                max(prior_available_at) FILTER (WHERE lag_year=2) AS prior_2y_available_at,
                max(prior_available_at) FILTER (WHERE lag_year=3) AS prior_3y_available_at,
                max(prior_source) FILTER (WHERE lag_year=1) AS prior_1y_source,
                max(prior_source) FILTER (WHERE lag_year=2) AS prior_2y_source,
                max(prior_source) FILTER (WHERE lag_year=3) AS prior_3y_source
            FROM prior_candidates
            WHERE lag_rank = 1
            GROUP BY security_id,trade_date
            HAVING count(*) = 3 AND avg(prior_capex) > 0
        ),
        raw AS (
            SELECT
                *,
                current_capex/baseline_capex-1 AS abnormal_ratio,
                1-current_capex/baseline_capex AS raw_value
            FROM baseline
            WHERE abs(current_capex/baseline_capex-1) <= ?
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY trade_date) AS names_on_date
            FROM raw
            WHERE isfinite(raw_value)
        ),
        bounded AS (
            SELECT
                *,
                quantile_cont(raw_value,?) OVER (PARTITION BY trade_date) AS lower_bound,
                quantile_cont(raw_value,?) OVER (PARTITION BY trade_date) AS upper_bound
            FROM breadth
            WHERE names_on_date >= ?
        ),
        standardized AS (
            SELECT
                *,
                greatest(lower_bound,least(upper_bound,raw_value)) AS winsorized_value
            FROM bounded
        ),
        final_rows AS (
            SELECT
                *,
                avg(winsorized_value) OVER (PARTITION BY trade_date) AS cross_section_mean,
                stddev_samp(winsorized_value) OVER (PARTITION BY trade_date)
                    AS cross_section_std
            FROM standardized
        )
        SELECT
            sha256(concat_ws('|',?,'{FACTOR_ID}',security_id,cast(trade_date AS VARCHAR))),
            '{FACTOR_ID}',
            '{FACTOR_NAME}',
            'fundamental_investment',
            security_id,
            symbol,
            trade_date,
            raw_value,
            (winsorized_value-cross_section_mean)/cross_section_std,
            decision_available_at,
            ?,
            cast(to_json(struct_pack(
                method := 'pit_low_abnormal_capital_investment',
                formula := '1-current_ttm_capex/mean(prior_1y,prior_2y,prior_3y)',
                orientation := 'lower_abnormal_investment_is_preferred',
                maximum_current_age_days := ?,
                lag_tolerance_days := ?,
                maximum_absolute_abnormal_ratio := ?,
                minimum_names_per_date := ?,
                winsor_limit := ?,
                current_ttm_point_id := current_ttm_point_id,
                current_capex := current_capex,
                current_period_end := current_period_end,
                current_available_at := current_capex_available_at,
                current_source := current_capex_source,
                current_input_statement_point_ids_json := current_input_statement_point_ids_json,
                current_input_accessions_json := current_input_accessions_json,
                baseline_capex := baseline_capex,
                prior_1y_id := prior_1y_id,
                prior_1y_capex := prior_1y_capex,
                prior_1y_period_end := prior_1y_period_end,
                prior_1y_available_at := prior_1y_available_at,
                prior_1y_source := prior_1y_source,
                prior_2y_id := prior_2y_id,
                prior_2y_capex := prior_2y_capex,
                prior_2y_period_end := prior_2y_period_end,
                prior_2y_available_at := prior_2y_available_at,
                prior_2y_source := prior_2y_source,
                prior_3y_id := prior_3y_id,
                prior_3y_capex := prior_3y_capex,
                prior_3y_period_end := prior_3y_period_end,
                prior_3y_available_at := prior_3y_available_at,
                prior_3y_source := prior_3y_source,
                market_cap_id := market_cap_id,
                market_cap := market_cap,
                universe_id := '{options.universe_id}',
                universe_valid_from := universe_valid_from,
                universe_valid_to := universe_valid_to,
                universe_available_at := universe_available_at,
                universe_source := universe_source
            )) AS VARCHAR),
            true,
            ?,
            ?
        FROM final_rows
        WHERE cross_section_std > 0
          AND isfinite((winsorized_value-cross_section_mean)/cross_section_std)
    """
    insert_params = [
        *date_params,
        options.universe_id,
        options.maximum_current_age_days,
        options.lag_tolerance_days,
        options.maximum_absolute_abnormal_ratio,
        options.winsor_limit,
        1.0-options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids_json,
        options.maximum_current_age_days,
        options.lag_tolerance_days,
        options.maximum_absolute_abnormal_ratio,
        options.minimum_names_per_date,
        options.winsor_limit,
        options.run_id,
        options.source,
    ]
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
            delete_params,
        )
        store.con.execute(sql,insert_params)
    row = store.con.execute(
        f"SELECT count(*) FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
        delete_params,
    ).fetchone()
    return int(row[0])
