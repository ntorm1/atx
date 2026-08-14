"""Point-in-time annual R&D-to-market-equity factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .connection import DuckDBStore
from .universe import DEFAULT_UNIVERSE_ID
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT R&D intensity v1"
FACTOR_ID = "valuation_rd_to_market_equity"
FACTOR_NAME = "PIT annual R&D-to-market equity"


@dataclass(frozen=True)
class RdIntensityOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_fundamental_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_rd_intensity_values(
    store: DuckDBStore,
    options: RdIntensityOptions | None = None,
) -> int:
    """Materialize monthly PIT annual R&D expense divided by market equity."""

    options = options or RdIntensityOptions()
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
        ["dataset:market_cap", "metric:rd_expense", f"universe:{options.universe_id}"]
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
        annual_rd AS (
            SELECT
                security_id,
                accession_number,
                period_end,
                arg_max(value,(available_at,revision_sequence,statement_point_id))
                    AS rd_expense,
                arg_max(statement_point_id,
                        (available_at,revision_sequence,statement_point_id))
                    AS rd_statement_point_id,
                max(available_at) AS rd_available_at,
                arg_max(source,(available_at,revision_sequence,statement_point_id))
                    AS rd_source
            FROM fundamental_statement_points
            WHERE canonical_metric = 'rd_expense'
              AND unit = 'USD'
              AND period_start IS NOT NULL
              AND period_end IS NOT NULL
              AND date_diff('day',period_start,period_end) + 1 BETWEEN 330 AND 380
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
              AND value > 0
              AND isfinite(value)
              AND accession_number IS NOT NULL
            GROUP BY security_id,accession_number,period_end
        ),
        candidates AS (
            SELECT
                m.*,
                r.accession_number,
                r.period_end,
                r.rd_expense,
                r.rd_statement_point_id,
                r.rd_available_at,
                r.rd_source,
                row_number() OVER (
                    PARTITION BY m.security_id,m.trade_date
                    ORDER BY r.period_end DESC,r.rd_available_at DESC,
                             r.accession_number DESC,r.rd_statement_point_id DESC
                ) AS rd_rank
            FROM governed m
            JOIN annual_rd r
              ON r.security_id = m.security_id
             AND r.period_end <= m.trade_date
             AND r.rd_available_at <= m.available_at
             AND m.trade_date - r.period_end <= ?
            WHERE m.universe_rank = 1
        ),
        raw AS (
            SELECT
                *,
                rd_expense / market_cap AS raw_value,
                greatest(
                    available_at,
                    rd_available_at,
                    coalesce(universe_available_at,TIMESTAMP '1900-01-01')
                ) AS decision_available_at
            FROM candidates
            WHERE rd_rank = 1
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY trade_date) AS names_on_date
            FROM raw
            WHERE raw_value > 0 AND isfinite(raw_value)
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
            'fundamental_valuation',
            security_id,
            symbol,
            trade_date,
            raw_value,
            (winsorized_value-cross_section_mean)/cross_section_std,
            decision_available_at,
            ?,
            cast(to_json(struct_pack(
                method := 'pit_annual_rd_to_market_equity',
                formula := 'annual_rd_expense/market_cap',
                orientation := 'higher_rd_intensity_is_preferred',
                maximum_fundamental_age_days := ?,
                minimum_names_per_date := ?,
                winsor_limit := ?,
                missing_rd_imputed_as_zero := false,
                market_cap_id := market_cap_id,
                market_cap := market_cap,
                market_cap_available_at := available_at,
                market_cap_input_lineage_json := input_lineage_json,
                rd_accession_number := accession_number,
                rd_statement_point_id := rd_statement_point_id,
                rd_expense := rd_expense,
                rd_period_end := period_end,
                rd_available_at := rd_available_at,
                rd_source := rd_source,
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
        options.maximum_fundamental_age_days,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids_json,
        options.maximum_fundamental_age_days,
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
