"""Point-in-time enterprise-yield research features."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .universe import DEFAULT_UNIVERSE_ID
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT enterprise yield v1"
VARIANT_FACTOR_IDS = {
    "ebit": "valuation_enterprise_yield_ebit",
    "gross_profit": "valuation_gross_profit_enterprise_yield",
    "operating_cash_flow": "valuation_operating_cash_flow_enterprise_yield",
    "sales": "valuation_enterprise_yield_sales",
}
VARIANT_METRICS = {
    "ebit": "operating_income",
    "gross_profit": "gross_profit",
    "operating_cash_flow": "operating_cash_flow",
    "sales": "revenue",
}
VARIANT_NAMES = {
    "ebit": "PIT operating-income enterprise yield",
    "gross_profit": "PIT gross-profit enterprise yield",
    "operating_cash_flow": "PIT operating-cash-flow enterprise yield",
    "sales": "PIT revenue enterprise yield",
}
VARIANT_INPUT_IDS = {
    "ebit": "metric:operating_income",
    "gross_profit": "factor:profitability_gross_profitability",
    "operating_cash_flow": "factor:profitability_operating_cash_flow_to_assets",
    "sales": "metric:revenue",
}
_OUTPUT_COLUMNS = [
    "factor_value_id",
    "factor_id",
    "factor_name",
    "family",
    "security_id",
    "symbol",
    "as_of_date",
    "raw_value",
    "value",
    "available_at",
    "input_ids_json",
    "input_lineage_json",
    "is_latest_revision",
    "run_id",
    "source",
]


@dataclass(frozen=True)
class EnterpriseYieldOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_fundamental_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, factor_id: str, security_id: str, as_of_date: Any) -> str:
    return hashlib.sha256(
        "|".join(str(part) for part in (source, factor_id, security_id, as_of_date)).encode()
    ).hexdigest()


def load_enterprise_yield_inputs(
    store: DuckDBStore,
    options: EnterpriseYieldOptions | None = None,
) -> pd.DataFrame:
    """Resolve monthly EV and latest visible EBIT, sales, or annual gross profit."""

    options = options or EnterpriseYieldOptions()
    predicates: list[str] = []
    params: list[object] = [options.universe_id, options.maximum_fundamental_age_days]
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    date_sql = " AND " + " AND ".join(predicates) if predicates else ""
    sql = f"""
        WITH ev_months AS (
            SELECT
                e.*,
                row_number() OVER (
                    PARTITION BY e.security_id, year(e.trade_date), month(e.trade_date)
                    ORDER BY e.trade_date DESC, e.available_at DESC, e.enterprise_value_id DESC
                ) AS month_rank
            FROM enterprise_value e
            WHERE e.is_latest_revision
              AND e.enterprise_value > 0
              AND isfinite(e.enterprise_value)
              AND e.available_at <= e.market_cap_available_at
        ),
        rebalances AS (
            SELECT * EXCLUDE (month_rank)
            FROM ev_months
            WHERE month_rank = 1 {date_sql}
        ),
        governed AS (
            SELECT
                e.*,
                u.valid_from AS universe_valid_from,
                u.valid_to AS universe_valid_to,
                u.available_at AS universe_available_at,
                u.source AS universe_source,
                row_number() OVER (
                    PARTITION BY e.security_id, e.trade_date
                    ORDER BY u.valid_from DESC, u.available_at DESC NULLS LAST,
                             u.source_loaded_at DESC, u.source DESC
                ) AS universe_rank
            FROM rebalances e
            JOIN universe_membership u
              ON u.universe_id = ?
             AND u.security_id = e.security_id
             AND u.valid_from <= e.trade_date
             AND (u.valid_to IS NULL OR u.valid_to >= e.trade_date)
             AND u.as_of_date <= e.trade_date
             AND u.is_member
             AND u.is_latest_revision
             AND (u.available_at IS NULL OR u.available_at <= e.market_cap_available_at)
        ),
        ttm_events AS (
            SELECT
                t.security_id,
                t.ttm_end_date AS period_end,
                t.canonical_metric,
                CASE t.canonical_metric
                    WHEN 'operating_income' THEN 'ebit'
                    WHEN 'operating_cash_flow' THEN 'operating_cash_flow'
                    ELSE 'sales'
                END AS variant,
                arg_max(t.ttm_value, (t.available_at, t.revision_sequence, t.ttm_point_id)) AS metric_value,
                arg_max(t.ttm_point_id, (t.available_at, t.revision_sequence, t.ttm_point_id)) AS metric_id,
                max(t.available_at) AS metric_available_at,
                arg_max(t.source, (t.available_at, t.revision_sequence, t.ttm_point_id)) AS metric_source
            FROM fundamental_ttm_points t
            WHERE t.is_latest_revision
              AND t.canonical_metric IN ('operating_cash_flow', 'operating_income', 'revenue')
              AND t.ttm_value IS NOT NULL
              AND isfinite(t.ttm_value)
              AND t.available_at IS NOT NULL
            GROUP BY t.security_id, t.ttm_end_date, t.canonical_metric
        ),
        gross_profit_events AS (
            SELECT
                f.security_id,
                cast(json_extract_string(f.input_lineage_json, '$.gross_profit.period_end') AS DATE)
                    AS period_end,
                'gross_profit' AS canonical_metric,
                'gross_profit' AS variant,
                cast(json_extract_string(f.input_lineage_json, '$.gross_profit.value') AS DOUBLE)
                    AS metric_value,
                json_extract_string(f.input_lineage_json, '$.gross_profit.id') AS metric_id,
                f.available_at AS metric_available_at,
                f.source AS metric_source
            FROM fundamental_factor_values f
            WHERE f.factor_id = 'profitability_gross_profitability'
              AND f.is_latest_revision
              AND f.available_at IS NOT NULL
              AND json_extract_string(f.input_lineage_json, '$.gross_profit.id') IS NOT NULL
              AND cast(
                    json_extract_string(f.input_lineage_json, '$.gross_profit.value') AS DOUBLE
                  ) > 0
        ),
        metric_events AS (
            SELECT * FROM ttm_events
            UNION ALL
            SELECT * FROM gross_profit_events
        ),
        candidates AS (
            SELECT
                g.*,
                t.variant,
                t.period_end,
                t.metric_value,
                t.metric_id,
                t.metric_available_at,
                t.metric_source,
                row_number() OVER (
                    PARTITION BY g.security_id, g.trade_date, t.variant
                    ORDER BY t.period_end DESC, t.metric_available_at DESC, t.metric_id DESC
                ) AS metric_rank
            FROM governed g
            JOIN metric_events t
              ON t.security_id = g.security_id
             AND t.period_end <= g.trade_date
             AND t.metric_available_at <= g.market_cap_available_at
             AND g.trade_date - t.period_end <= ?
            WHERE g.universe_rank = 1
              AND t.metric_value > 0
        )
        SELECT
            * EXCLUDE (universe_rank, metric_rank),
            greatest(
                available_at,
                market_cap_available_at,
                metric_available_at,
                coalesce(universe_available_at, TIMESTAMP '1900-01-01')
            ) AS decision_available_at
        FROM candidates
        WHERE metric_rank = 1
        ORDER BY variant, trade_date, security_id
    """
    return store.con.execute(sql, params).df()


def _lineage(row: pd.Series, options: EnterpriseYieldOptions) -> str:
    variant = str(row["variant"])
    metric = VARIANT_METRICS[variant]
    formula = (
        "annual_gross_profit/enterprise_value"
        if variant == "gross_profit"
        else f"{metric}_ttm/enterprise_value"
    )
    return json_dumps(
        {
            "method": "pit_enterprise_yield",
            "variant": row["variant"],
            "formula": formula,
            "orientation": "higher_yield_is_preferred",
            "research_contract": {
                "maximum_fundamental_age_days": options.maximum_fundamental_age_days,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "minimum_names_per_date": options.minimum_names_per_date,
                "missing_components_imputed": False,
            },
            "enterprise_value": {
                "enterprise_value_id": row.get("enterprise_value_id"),
                "value": row.get("enterprise_value"),
                "period_end": row.get("period_end"),
                "available_at": row.get("available_at"),
                "input_lineage_json": row.get("input_lineage_json"),
            },
            metric: {
                "ttm_point_id": row.get("metric_id"),
                "value": row.get("metric_value"),
                "period_end": row.get("period_end"),
                "available_at": row.get("metric_available_at"),
                "source": row.get("metric_source"),
            },
            "universe": {
                "universe_id": options.universe_id,
                "valid_from": row.get("universe_valid_from"),
                "valid_to": row.get("universe_valid_to"),
                "available_at": row.get("universe_available_at"),
                "source": row.get("universe_source"),
            },
        }
    )


def compute_enterprise_yield_rows(
    inputs: pd.DataFrame,
    options: EnterpriseYieldOptions | None = None,
) -> pd.DataFrame:
    """Compute, winsorize, and date-standardize enterprise-yield variants."""

    options = options or EnterpriseYieldOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "variant",
        "security_id",
        "trade_date",
        "decision_available_at",
        "enterprise_value",
        "metric_value",
        "metric_id",
        "period_end",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"Enterprise-yield inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    for column in ("enterprise_value", "metric_value"):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=["variant", "security_id", "as_of_date", "available_at", "enterprise_value", "metric_value"]
    )
    rows = rows[
        (rows["enterprise_value"] > 0)
        & (rows["metric_value"] > 0)
        & rows["enterprise_value"].map(math.isfinite)
        & rows["metric_value"].map(math.isfinite)
    ].copy()
    rows["raw_value"] = rows["metric_value"] / rows["enterprise_value"]
    rows = rows[rows["raw_value"].map(math.isfinite)].copy()
    breadth = rows.groupby(["variant", "as_of_date"])["security_id"].transform("nunique")
    rows = rows[breadth >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows["factor_id"] = rows["variant"].map(VARIANT_FACTOR_IDS)
    rows["factor_name"] = rows["variant"].map(VARIANT_NAMES)
    rows["family"] = "fundamental_valuation"
    rows = winsorize(
        rows,
        value_column="raw_value",
        output_column="winsorized_value",
        partition_columns=("factor_id", "as_of_date"),
        limits=options.winsor_limit,
    )
    rows = zscore(
        rows,
        value_column="winsorized_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = rows["variant"].map(
        lambda variant: json_dumps(
            [
                "dataset:enterprise_value",
                VARIANT_INPUT_IDS[str(variant)],
                f"universe:{options.universe_id}",
            ]
        )
    )
    rows["input_lineage_json"] = rows.apply(lambda row: _lineage(row, options), axis=1)
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    if "symbol" not in rows:
        rows["symbol"] = pd.NA
    rows["factor_value_id"] = [
        _factor_value_id(options.source, factor_id, security_id, as_of_date)
        for factor_id, security_id, as_of_date in zip(
            rows["factor_id"], rows["security_id"], rows["as_of_date"], strict=True
        )
    ]
    return (
        rows[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["factor_id", "as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_enterprise_yield_values(
    store: DuckDBStore,
    options: EnterpriseYieldOptions | None = None,
) -> int:
    options = options or EnterpriseYieldOptions()
    store.initialize()
    rows = compute_enterprise_yield_rows(load_enterprise_yield_inputs(store, options), options)
    factor_ids = (
        VARIANT_FACTOR_IDS["ebit"],
        VARIANT_FACTOR_IDS["sales"],
    )
    rows = rows[rows["factor_id"].isin(factor_ids)].copy()
    placeholders = ", ".join("?" for _ in factor_ids)
    predicates = ["source = ?", f"factor_id IN ({placeholders})"]
    params: list[object] = [options.source, *factor_ids]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}", params
        )
        if not rows.empty:
            insert_frame(store, rows, "fundamental_factor_values", "enterprise_yield_insert")
    return (
        len(rows)
        + refresh_gross_profit_enterprise_yield_set_based(store, options)
        + refresh_operating_cash_flow_enterprise_yield_set_based(store, options)
    )


def _refresh_lineaged_enterprise_yield_set_based(
    store: DuckDBStore,
    *,
    parent_factor_id: str,
    factor_id: str,
    factor_name: str,
    variant: str,
    metric_name: str,
    value_json_path: str,
    period_json_path: str,
    metric_id_json_path: str,
    formula: str,
    options: EnterpriseYieldOptions,
) -> int:
    """Build an enterprise yield from an exact value recorded in parent lineage."""

    store.initialize()
    predicates = [f"f.factor_id = '{parent_factor_id}'", "f.is_latest_revision"]
    date_params: list[object] = []
    delete_predicates = ["factor_id = ?", "source = ?"]
    delete_params: list[object] = [factor_id, options.source]
    if options.start_date is not None:
        predicates.append("f.as_of_date >= ?")
        date_params.append(options.start_date)
        delete_predicates.append("as_of_date >= ?")
        delete_params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("f.as_of_date <= ?")
        date_params.append(options.end_date)
        delete_predicates.append("as_of_date <= ?")
        delete_params.append(options.end_date)

    sql = f"""
        INSERT INTO fundamental_factor_values (
            factor_value_id,factor_id,factor_name,family,security_id,symbol,
            as_of_date,raw_value,value,available_at,input_ids_json,
            input_lineage_json,is_latest_revision,run_id,source
        )
        WITH source_rows AS (
            SELECT
                f.security_id,
                f.symbol,
                f.as_of_date,
                f.available_at AS factor_available_at,
                f.factor_value_id AS parent_factor_value_id,
                f.input_lineage_json AS parent_factor_lineage,
                cast(json_extract_string(
                    f.input_lineage_json, '{value_json_path}'
                ) AS DOUBLE) AS metric_value,
                cast(json_extract_string(
                    f.input_lineage_json, '{period_json_path}'
                ) AS DATE) AS period_end,
                json_extract_string(
                    f.input_lineage_json, '{metric_id_json_path}'
                ) AS metric_source_id,
                e.enterprise_value,
                e.enterprise_value_id,
                e.available_at AS enterprise_value_available_at,
                e.market_cap_available_at,
                e.input_lineage_json AS enterprise_value_lineage,
                row_number() OVER (
                    PARTITION BY f.security_id, f.as_of_date
                    ORDER BY e.available_at DESC, e.enterprise_value_id DESC
                ) AS enterprise_value_rank
            FROM fundamental_factor_values f
            JOIN enterprise_value e
              ON e.security_id = f.security_id
             AND e.trade_date = f.as_of_date
            WHERE {' AND '.join(predicates)}
              AND e.is_latest_revision
              AND e.enterprise_value > 0
              AND isfinite(e.enterprise_value)
              AND e.available_at <= e.market_cap_available_at
              AND f.available_at <= e.market_cap_available_at
        ),
        raw AS (
            SELECT
                *,
                metric_value / enterprise_value AS raw_value,
                greatest(
                    factor_available_at,
                    enterprise_value_available_at,
                    market_cap_available_at
                ) AS decision_available_at
            FROM source_rows
            WHERE enterprise_value_rank = 1
              AND metric_value > 0
              AND isfinite(metric_value)
              AND metric_source_id IS NOT NULL
              AND as_of_date - period_end <= ?
        ),
        breadth AS (
            SELECT *, count(*) OVER (PARTITION BY as_of_date) AS names_on_date
            FROM raw
            WHERE isfinite(raw_value)
        ),
        bounded AS (
            SELECT
                *,
                quantile_cont(raw_value, ?) OVER (PARTITION BY as_of_date) AS lower_bound,
                quantile_cont(raw_value, ?) OVER (PARTITION BY as_of_date) AS upper_bound
            FROM breadth
            WHERE names_on_date >= ?
        ),
        standardized AS (
            SELECT
                *,
                greatest(lower_bound, least(upper_bound, raw_value)) AS winsorized_value
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
            sha256(concat_ws(
                '|', ?, '{factor_id}',
                security_id, cast(as_of_date AS VARCHAR)
            )),
            '{factor_id}',
            '{factor_name}',
            'fundamental_valuation',
            security_id,
            symbol,
            as_of_date,
            raw_value,
            (winsorized_value - cross_section_mean) / cross_section_std,
            decision_available_at,
            ?,
            cast(to_json(struct_pack(
                method := 'pit_lineaged_enterprise_yield',
                variant := '{variant}',
                formula := '{formula}',
                orientation := 'higher_yield_is_preferred',
                maximum_fundamental_age_days := ?,
                minimum_names_per_date := ?,
                winsor_limit := ?,
                parent_factor_id := '{parent_factor_id}',
                parent_factor_value_id := parent_factor_value_id,
                metric_name := '{metric_name}',
                metric_value := metric_value,
                metric_period_end := period_end,
                metric_source_id := metric_source_id,
                metric_available_at := factor_available_at,
                parent_factor_input_lineage_json := parent_factor_lineage,
                enterprise_value_id := enterprise_value_id,
                enterprise_value := enterprise_value,
                enterprise_value_available_at := enterprise_value_available_at,
                enterprise_value_input_lineage_json := enterprise_value_lineage
            )) AS VARCHAR),
            true,
            ?,
            ?
        FROM final_rows
        WHERE cross_section_std > 0
          AND isfinite((winsorized_value - cross_section_mean) / cross_section_std)
    """
    insert_params = [
        *date_params,
        options.maximum_fundamental_age_days,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        json_dumps(
            [
                "dataset:enterprise_value",
                f"factor:{parent_factor_id}",
                f"universe:{options.universe_id}",
            ]
        ),
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
        store.con.execute(sql, insert_params)
    row = store.con.execute(
        f"""
        SELECT count(*) FROM fundamental_factor_values
        WHERE {' AND '.join(delete_predicates)}
        """,
        delete_params,
    ).fetchone()
    return int(row[0])


def refresh_gross_profit_enterprise_yield_set_based(
    store: DuckDBStore,
    options: EnterpriseYieldOptions | None = None,
) -> int:
    """Materialize annual gross-profit/EV without rewriting other variants."""

    options = options or EnterpriseYieldOptions()
    return _refresh_lineaged_enterprise_yield_set_based(
        store,
        parent_factor_id="profitability_gross_profitability",
        factor_id=VARIANT_FACTOR_IDS["gross_profit"],
        factor_name=VARIANT_NAMES["gross_profit"],
        variant="gross_profit",
        metric_name="gross_profit",
        value_json_path="$.gross_profit.value",
        period_json_path="$.gross_profit.period_end",
        metric_id_json_path="$.gross_profit.id",
        formula="annual_gross_profit/enterprise_value",
        options=options,
    )


def refresh_operating_cash_flow_enterprise_yield_set_based(
    store: DuckDBStore,
    options: EnterpriseYieldOptions | None = None,
) -> int:
    """Materialize TTM operating-cash-flow/EV without rewriting other variants."""

    options = options or EnterpriseYieldOptions()
    return _refresh_lineaged_enterprise_yield_set_based(
        store,
        parent_factor_id="profitability_operating_cash_flow_to_assets",
        factor_id=VARIANT_FACTOR_IDS["operating_cash_flow"],
        factor_name=VARIANT_NAMES["operating_cash_flow"],
        variant="operating_cash_flow",
        metric_name="operating_cash_flow",
        value_json_path="$.ttm.operating_cash_flow_ttm",
        period_json_path="$.ttm.period_end",
        metric_id_json_path="$.ttm.operating_cash_flow_ttm_id",
        formula="operating_cash_flow_ttm/enterprise_value",
        options=options,
    )
