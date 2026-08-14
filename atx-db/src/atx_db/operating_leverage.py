"""Point-in-time Novy-Marx operating-leverage factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .connection import DuckDBStore
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT operating leverage v1"
PARENT_FACTOR_ID = "profitability_operating_profitability"
FACTOR_ID = "risk_operating_leverage"
FACTOR_NAME = "PIT operating leverage"


@dataclass(frozen=True)
class OperatingLeverageOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_raw_value: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_operating_leverage_values(
    store: DuckDBStore,
    options: OperatingLeverageOptions | None = None,
) -> int:
    """Materialize annual operating costs divided by same-filing assets."""

    options = options or OperatingLeverageOptions()
    store.initialize()
    predicates = ["f.factor_id = ?", "f.is_latest_revision"]
    source_params: list[object] = [PARENT_FACTOR_ID]
    delete_predicates = ["factor_id = ?", "source = ?"]
    delete_params: list[object] = [FACTOR_ID,options.source]
    if options.start_date is not None:
        predicates.append("f.as_of_date >= ?")
        source_params.append(options.start_date)
        delete_predicates.append("as_of_date >= ?")
        delete_params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("f.as_of_date <= ?")
        source_params.append(options.end_date)
        delete_predicates.append("as_of_date <= ?")
        delete_params.append(options.end_date)
    input_ids_json = json_dumps([f"factor:{PARENT_FACTOR_ID}"])
    sql = f"""
        INSERT INTO fundamental_factor_values (
            factor_value_id,factor_id,factor_name,family,security_id,symbol,
            as_of_date,raw_value,value,available_at,input_ids_json,
            input_lineage_json,is_latest_revision,run_id,source
        )
        WITH inputs AS (
            SELECT
                f.factor_value_id AS parent_factor_value_id,
                f.security_id,
                f.symbol,
                f.as_of_date,
                f.available_at,
                try_cast(json_extract_string(
                    f.input_lineage_json,'$.operating_profitability.cogs.value'
                ) AS DOUBLE) AS reported_cogs,
                json_extract_string(
                    f.input_lineage_json,'$.operating_profitability.cogs.id'
                ) AS reported_cogs_id,
                try_cast(json_extract_string(
                    f.input_lineage_json,'$.operating_profitability.sga.value'
                ) AS DOUBLE) AS sga,
                json_extract_string(
                    f.input_lineage_json,'$.operating_profitability.sga.id'
                ) AS sga_id,
                try_cast(json_extract_string(
                    f.input_lineage_json,'$.operating_profitability.revenue.value'
                ) AS DOUBLE) AS revenue,
                json_extract_string(
                    f.input_lineage_json,'$.operating_profitability.revenue.id'
                ) AS revenue_id,
                try_cast(json_extract_string(
                    f.input_lineage_json,'$.gross_profit.value'
                ) AS DOUBLE) AS gross_profit,
                json_extract_string(f.input_lineage_json,'$.gross_profit.id')
                    AS gross_profit_id,
                try_cast(json_extract_string(
                    f.input_lineage_json,'$.total_assets.value'
                ) AS DOUBLE) AS total_assets,
                json_extract_string(f.input_lineage_json,'$.total_assets.id')
                    AS total_assets_id,
                json_extract_string(f.input_lineage_json,'$.gross_profit.period_end')
                    AS annual_period_end
            FROM fundamental_factor_values f
            WHERE {' AND '.join(predicates)}
        ),
        resolved AS (
            SELECT
                *,
                CASE
                    WHEN isfinite(reported_cogs) THEN reported_cogs
                    WHEN isfinite(revenue) AND isfinite(gross_profit)
                        THEN revenue-gross_profit
                END AS resolved_cogs,
                CASE
                    WHEN isfinite(reported_cogs) THEN 'reported_cogs'
                    ELSE 'revenue_minus_gross_profit'
                END AS cogs_method
            FROM inputs
            WHERE isfinite(sga)
              AND sga > 0
              AND isfinite(total_assets)
              AND total_assets > 0
        ),
        raw AS (
            SELECT
                *,
                (resolved_cogs+sga)/total_assets AS raw_value
            FROM resolved
            WHERE resolved_cogs > 0
              AND isfinite(resolved_cogs)
              AND (resolved_cogs+sga)/total_assets > 0
              AND (resolved_cogs+sga)/total_assets <= ?
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY as_of_date) AS names_on_date
            FROM raw
            WHERE isfinite(raw_value)
        ),
        bounded AS (
            SELECT
                *,
                quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date) AS lower_bound,
                quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date) AS upper_bound
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
                avg(winsorized_value) OVER (PARTITION BY as_of_date) AS cross_section_mean,
                stddev_samp(winsorized_value) OVER (PARTITION BY as_of_date)
                    AS cross_section_std
            FROM standardized
        )
        SELECT
            sha256(concat_ws('|',?,'{FACTOR_ID}',security_id,cast(as_of_date AS VARCHAR))),
            '{FACTOR_ID}',
            '{FACTOR_NAME}',
            'fundamental_risk',
            security_id,
            symbol,
            as_of_date,
            raw_value,
            (winsorized_value-cross_section_mean)/cross_section_std,
            available_at,
            ?,
            cast(to_json(struct_pack(
                method := 'novy_marx_operating_leverage_pit',
                formula := '(cogs+sga)/total_assets',
                orientation := 'higher_operating_leverage_is_preferred',
                maximum_raw_value := ?,
                minimum_names_per_date := ?,
                winsor_limit := ?,
                parent_factor_id := '{PARENT_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                annual_period_end := annual_period_end,
                cogs_method := cogs_method,
                reported_cogs := reported_cogs,
                reported_cogs_id := reported_cogs_id,
                resolved_cogs := resolved_cogs,
                sga := sga,
                sga_id := sga_id,
                revenue := revenue,
                revenue_id := revenue_id,
                gross_profit := gross_profit,
                gross_profit_id := gross_profit_id,
                total_assets := total_assets,
                total_assets_id := total_assets_id
            )) AS VARCHAR),
            true,
            ?,
            ?
        FROM final_rows
        WHERE cross_section_std > 0
          AND isfinite((winsorized_value-cross_section_mean)/cross_section_std)
    """
    insert_params = [
        *source_params,
        options.maximum_raw_value,
        options.winsor_limit,
        1.0-options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids_json,
        options.maximum_raw_value,
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
