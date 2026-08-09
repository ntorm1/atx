"""Point-in-time Ball et al. cash operating profitability features.

The exact balance-sheet construction removes operating working-capital accruals
from annual operating profit and scales both measures by prior-year assets.  It
is deliberately separate from the Fama--French operating-profitability feature,
whose numerator includes interest and whose denominator is book equity.
"""
from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from typing import Any, cast

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT cash profitability v1"
FACTOR_IDS = (
    "profitability_ball_operating_profitability",
    "profitability_cash_operating_profitability",
    "quality_low_operating_working_capital_accruals",
)

FACTOR_METADATA: dict[str, dict[str, object]] = {
    "profitability_ball_operating_profitability": {
        "factor_name": "PIT Ball operating profitability",
        "family": "fundamental_profitability",
        "raw_column": "ball_operating_profitability",
        "direction": 1.0,
    },
    "profitability_cash_operating_profitability": {
        "factor_name": "PIT cash operating profitability",
        "family": "fundamental_profitability",
        "raw_column": "cash_operating_profitability",
        "direction": 1.0,
    },
    "quality_low_operating_working_capital_accruals": {
        "factor_name": "PIT low operating working-capital accruals",
        "family": "fundamental_quality",
        "raw_column": "operating_working_capital_accruals",
        "direction": -1.0,
    },
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

_WORKING_CAPITAL_METRICS = (
    "ar",
    "inventory",
    "prepaid_expense",
    "deferred_revenue",
    "ap",
    "accrued_liabilities",
)


@dataclass(frozen=True)
class CashProfitabilityOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_market_cap_usd: float = 100_000_000.0
    minimum_adv21_usd: float = 1_000_000.0
    maximum_fundamental_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, factor_id: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, factor_id, security_id, as_of_date))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _date_filter(options: CashProfitabilityOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_cash_profitability_inputs(
    store: DuckDBStore,
    options: CashProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Assemble monthly cash-profitability inputs without revision look-ahead."""

    options = options or CashProfitabilityOptions()
    date_sql, date_params = _date_filter(options)
    value_columns = [
        "revenue",
        "cogs",
        "sga",
        "rd_expense",
        "total_assets",
        *_WORKING_CAPITAL_METRICS,
    ]
    pivot_columns: list[str] = []
    for metric in value_columns:
        duration_predicate = (
            " AND period_start IS NOT NULL"
            " AND period_end - period_start BETWEEN 329 AND 379"
            if metric in {"revenue", "cogs", "sga", "rd_expense"}
            else ""
        )
        pivot_columns.extend(
            [
                f"arg_max(value, (available_at, revision_sequence, statement_point_id)) "
                f"FILTER (WHERE canonical_metric = '{metric}'{duration_predicate}) AS {metric}",
                f"max(available_at) FILTER (WHERE canonical_metric = '{metric}'"
                f"{duration_predicate}) AS {metric}_available_at",
                f"arg_max(statement_point_id, (available_at, revision_sequence, statement_point_id)) "
                f"FILTER (WHERE canonical_metric = '{metric}'{duration_predicate}) AS {metric}_id",
            ]
        )
    metric_sql = ",\n                ".join(pivot_columns)
    metric_filter = ", ".join(f"'{metric}'" for metric in value_columns)

    sql = f"""
        WITH annual_facts AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                period_end,
                {metric_sql}
            FROM fundamental_statement_points
            WHERE canonical_metric IN ({metric_filter})
              AND period_end IS NOT NULL
              AND accession_number IS NOT NULL
              AND form IN ('10-K', '10-K/A', '20-F', '20-F/A', '40-F', '40-F/A')
            GROUP BY security_id, accession_number, period_end
        ),
        annual AS (
            SELECT
                *,
                greatest(
                    revenue_available_at,
                    cogs_available_at,
                    sga_available_at,
                    rd_expense_available_at,
                    total_assets_available_at,
                    ar_available_at,
                    inventory_available_at,
                    prepaid_expense_available_at,
                    deferred_revenue_available_at,
                    ap_available_at,
                    accrued_liabilities_available_at
                ) AS annual_available_at,
                greatest(
                    total_assets_available_at,
                    ar_available_at,
                    inventory_available_at,
                    prepaid_expense_available_at,
                    deferred_revenue_available_at,
                    ap_available_at,
                    accrued_liabilities_available_at
                ) AS balance_available_at
            FROM annual_facts
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                arg_max("close", available_at) AS "close",
                arg_max(volume, available_at) AS volume,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (
                SELECT DISTINCT security_id
                FROM annual
                WHERE revenue IS NOT NULL AND (cogs IS NOT NULL OR sga IS NOT NULL)
            )
              AND "close" > 0
              AND trade_date IS NOT NULL
              AND available_at IS NOT NULL
            GROUP BY security_id, trade_date
        ),
        price_features AS (
            SELECT
                *,
                avg("close" * volume) OVER (
                    PARTITION BY security_id ORDER BY trade_date
                    ROWS BETWEEN 20 PRECEDING AND CURRENT ROW
                ) AS adv21_usd,
                row_number() OVER (
                    PARTITION BY security_id, year(trade_date), month(trade_date)
                    ORDER BY trade_date DESC
                ) AS month_rank
            FROM price_dedup
        ),
        rebalances AS (
            SELECT * FROM price_features
            WHERE month_rank = 1 {date_sql}
        ),
        share_dedup AS (
            SELECT
                security_id,
                available_at,
                arg_max(share_count, (effective_date, revision_sequence, share_history_id))
                    AS share_count,
                arg_max(share_history_id, (effective_date, revision_sequence, share_history_id))
                    AS share_history_id,
                max(effective_date) AS effective_date
            FROM shares_outstanding_history
            WHERE share_count_type = 'shares_outstanding' AND share_count > 0
            GROUP BY security_id, available_at
        ),
        current_candidates AS (
            SELECT
                p.*,
                a.* EXCLUDE (security_id),
                s.share_count,
                s.share_history_id,
                s.available_at AS share_available_at,
                s.effective_date,
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY a.period_end DESC, a.annual_available_at DESC,
                             a.accession_number DESC
                ) AS current_rank
            FROM rebalances p
            JOIN annual a
              ON a.security_id = p.security_id
             AND a.period_end <= p.trade_date
             AND a.annual_available_at <= p.price_available_at
             AND a.revenue IS NOT NULL
             AND (a.cogs IS NOT NULL OR a.sga IS NOT NULL)
            ASOF LEFT JOIN share_dedup s
              ON p.security_id = s.security_id
             AND p.price_available_at >= s.available_at
        ),
        current_annual AS (
            SELECT * FROM current_candidates
            WHERE current_rank = 1 AND effective_date <= trade_date
        ),
        prior_candidates AS (
            SELECT
                c.*,
                p.period_end AS prior_period_end,
                p.accession_number AS prior_accession_number,
                p.balance_available_at AS prior_balance_available_at,
                p.total_assets AS prior_total_assets,
                p.total_assets_available_at AS prior_total_assets_available_at,
                p.total_assets_id AS prior_total_assets_id,
                p.ar AS prior_ar,
                p.ar_available_at AS prior_ar_available_at,
                p.ar_id AS prior_ar_id,
                p.inventory AS prior_inventory,
                p.inventory_available_at AS prior_inventory_available_at,
                p.inventory_id AS prior_inventory_id,
                p.prepaid_expense AS prior_prepaid_expense,
                p.prepaid_expense_available_at AS prior_prepaid_expense_available_at,
                p.prepaid_expense_id AS prior_prepaid_expense_id,
                p.deferred_revenue AS prior_deferred_revenue,
                p.deferred_revenue_available_at AS prior_deferred_revenue_available_at,
                p.deferred_revenue_id AS prior_deferred_revenue_id,
                p.ap AS prior_ap,
                p.ap_available_at AS prior_ap_available_at,
                p.ap_id AS prior_ap_id,
                p.accrued_liabilities AS prior_accrued_liabilities,
                p.accrued_liabilities_available_at AS prior_accrued_liabilities_available_at,
                p.accrued_liabilities_id AS prior_accrued_liabilities_id,
                row_number() OVER (
                    PARTITION BY c.security_id, c.trade_date
                    ORDER BY p.period_end DESC, p.balance_available_at DESC,
                             p.accession_number DESC
                ) AS prior_rank
            FROM current_annual c
            JOIN annual p
              ON p.security_id = c.security_id
             AND p.period_end < c.period_end
             AND c.period_end - p.period_end BETWEEN 300 AND 430
             AND p.total_assets > 0
             AND p.balance_available_at <= c.price_available_at
        ),
        calculated AS (
            SELECT
                *,
                revenue - coalesce(cogs, 0) - coalesce(sga - coalesce(rd_expense, 0), 0)
                    AS operating_profit_numerator,
                coalesce(ar, 0) - coalesce(prior_ar, 0) AS delta_ar,
                coalesce(inventory, 0) - coalesce(prior_inventory, 0) AS delta_inventory,
                coalesce(prepaid_expense, 0) - coalesce(prior_prepaid_expense, 0)
                    AS delta_prepaid_expense,
                coalesce(deferred_revenue, 0) - coalesce(prior_deferred_revenue, 0)
                    AS delta_deferred_revenue,
                coalesce(ap, 0) - coalesce(prior_ap, 0) AS delta_ap,
                coalesce(accrued_liabilities, 0) - coalesce(prior_accrued_liabilities, 0)
                    AS delta_accrued_liabilities,
                "close" * share_count AS market_cap_usd
            FROM prior_candidates
            WHERE prior_rank = 1
        ),
        result AS (
            SELECT
                *,
                operating_profit_numerator / prior_total_assets
                    AS ball_operating_profitability,
                (
                    operating_profit_numerator
                    - delta_ar - delta_inventory - delta_prepaid_expense
                    + delta_deferred_revenue + delta_ap + delta_accrued_liabilities
                ) / prior_total_assets AS cash_operating_profitability,
                (
                    delta_ar + delta_inventory + delta_prepaid_expense
                    - delta_deferred_revenue - delta_ap - delta_accrued_liabilities
                ) / prior_total_assets AS operating_working_capital_accruals
            FROM calculated
        )
        SELECT * FROM result
        WHERE market_cap_usd >= ?
          AND adv21_usd >= ?
          AND trade_date - period_end <= ?
          AND ball_operating_profitability BETWEEN -20 AND 20
          AND cash_operating_profitability BETWEEN -20 AND 20
        ORDER BY trade_date, security_id
    """
    params = [
        *date_params,
        options.minimum_market_cap_usd,
        options.minimum_adv21_usd,
        options.maximum_fundamental_age_days,
    ]
    return store.con.execute(sql, params).df()


def _lineage(row: pd.Series, factor_id: str) -> str:
    fields = {
        metric: {
            "current_id": row.get(f"{metric}_id"),
            "current_value": row.get(metric),
            "prior_id": row.get(f"prior_{metric}_id"),
            "prior_value": row.get(f"prior_{metric}"),
            "missing_values_replaced_with_zero": bool(
                pd.isna(row.get(metric)) or pd.isna(row.get(f"prior_{metric}"))
            ),
        }
        for metric in _WORKING_CAPITAL_METRICS
    }
    return json_dumps(
        {
            "method": "ball_cash_operating_profitability_balance_sheet",
            "factor_id": factor_id,
            "decision": {
                "as_of_date": row["trade_date"],
                "available_at": row["price_available_at"],
                "security_id": row["security_id"],
                "close": row["close"],
                "adv21_usd": row["adv21_usd"],
                "market_cap_usd": row["market_cap_usd"],
            },
            "current_statement": {
                "period_end": row["period_end"],
                "accession_number": row["accession_number"],
                "available_at": row["annual_available_at"],
                "revenue_id": row["revenue_id"],
                "cogs_id": row["cogs_id"],
                "sga_id": row["sga_id"],
                "rd_expense_id": row["rd_expense_id"],
            },
            "prior_statement": {
                "period_end": row["prior_period_end"],
                "accession_number": row["prior_accession_number"],
                "available_at": row["prior_balance_available_at"],
                "total_assets_id": row["prior_total_assets_id"],
                "total_assets": row["prior_total_assets"],
            },
            "working_capital": fields,
            "formula": {
                "operating_profit": "revenue-cogs-coalesce(sga-rd_expense,0)",
                "cash_adjustment": "-dAR-dInventory-dPrepaid+dDeferredRevenue+dAP+dAccrued",
                "denominator": "prior_year_total_assets",
                "missing_balance_sheet_values": "zero",
            },
        }
    )


def compute_cash_profitability_rows(
    inputs: pd.DataFrame,
    options: CashProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Winsorize, orient, and z-score the cash-profitability family."""

    options = options or CashProfitabilityOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "price_available_at",
        "ball_operating_profitability",
        "cash_operating_profitability",
        "operating_working_capital_accruals",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"cash profitability inputs missing columns: {missing}")

    base = inputs.copy()
    base["trade_date"] = pd.to_datetime(base["trade_date"], errors="coerce").dt.date
    base["price_available_at"] = pd.to_datetime(base["price_available_at"], errors="coerce")
    parts: list[pd.DataFrame] = []
    for factor_id in FACTOR_IDS:
        metadata = FACTOR_METADATA[factor_id]
        raw_column = cast(str, metadata["raw_column"])
        direction = cast(float, metadata["direction"])
        part = base.dropna(
            subset=["security_id", "trade_date", "price_available_at", raw_column]
        ).copy()
        counts = part.groupby("trade_date")["security_id"].transform("nunique")
        part = part[counts >= options.minimum_names_per_date].copy()
        if part.empty:
            continue
        part["factor_id"] = factor_id
        part["factor_name"] = cast(str, metadata["factor_name"])
        part["family"] = cast(str, metadata["family"])
        part["raw_value"] = pd.to_numeric(part[raw_column], errors="coerce")
        part["oriented_value"] = direction * part["raw_value"]
        part["as_of_date"] = part["trade_date"]
        part = winsorize(
            part,
            value_column="oriented_value",
            output_column="winsorized_value",
            partition_columns=("factor_id", "as_of_date"),
            limits=options.winsor_limit,
        )
        part = zscore(
            part,
            value_column="winsorized_value",
            output_column="value",
            partition_columns=("factor_id", "as_of_date"),
        )
        part["available_at"] = part["price_available_at"]
        part["input_ids_json"] = json_dumps(
            [
                "metric:revenue",
                "metric:cogs",
                "metric:sga",
                "metric:rd_expense",
                "metric:total_assets",
                *[f"metric:{metric}" for metric in _WORKING_CAPITAL_METRICS],
            ]
        )
        part["input_lineage_json"] = part.apply(
            lambda row, current_factor_id=factor_id: _lineage(row, current_factor_id),
            axis=1,
        )
        part["is_latest_revision"] = True
        part["run_id"] = options.run_id
        part["source"] = options.source
        part["factor_value_id"] = [
            _factor_value_id(options.source, factor_id, security_id, as_of_date)
            for security_id, as_of_date in zip(
                part["security_id"], part["as_of_date"], strict=True
            )
        ]
        parts.append(part[_OUTPUT_COLUMNS])
    if not parts:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    return (
        pd.concat(parts, ignore_index=True)
        .dropna(subset=["value"])
        .sort_values(["factor_id", "as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_cash_profitability_values(
    store: DuckDBStore,
    options: CashProfitabilityOptions | None = None,
) -> int:
    options = options or CashProfitabilityOptions()
    store.initialize()
    inputs = load_cash_profitability_inputs(store, options)
    rows = compute_cash_profitability_rows(inputs, options)
    predicates = ["source = ?", f"factor_id IN ({', '.join('?' for _ in FACTOR_IDS)})"]
    params: list[object] = [options.source, *FACTOR_IDS]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}",
            params,
        )
        if not rows.empty:
            insert_frame(
                store,
                rows,
                "fundamental_factor_values",
                "cash_profitability_values_insert",
            )
    return len(rows)
