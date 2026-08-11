"""Point-in-time annual change in approximate net-operating-asset turnover."""

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

SOURCE_NAME = "atx-db PIT annual NOA-proxy turnover change v1"
FACTOR_ID = "efficiency_annual_noa_proxy_turnover_change"
FACTOR_NAME = "PIT annual change in NOA-proxy turnover"
FACTOR_FAMILY = "fundamental_efficiency"
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
class NoaProxyTurnoverChangeOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_fundamental_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _date_filter(options: NoaProxyTurnoverChangeOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_noa_proxy_turnover_change_inputs(
    store: DuckDBStore,
    options: NoaProxyTurnoverChangeOptions | None = None,
) -> pd.DataFrame:
    """Resolve exact annual revenue and NOA-proxy legs at governed month ends."""

    options = options or NoaProxyTurnoverChangeOptions()
    date_sql, date_params = _date_filter(options)
    sql = f"""
        WITH annual_filings AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                period_end,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'revenue'
                          AND period_type = 'duration'
                          AND period_start IS NOT NULL
                          AND period_end - period_start BETWEEN 329 AND 399
                    ) AS revenue,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'revenue'
                          AND period_type = 'duration'
                          AND period_start IS NOT NULL
                          AND period_end - period_start BETWEEN 329 AND 399
                    ) AS revenue_id,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'revenue'
                      AND period_type = 'duration'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 399
                ) AS revenue_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'stockholders_equity'
                          AND period_type = 'instant'
                    ) AS equity,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'stockholders_equity'
                          AND period_type = 'instant'
                    ) AS equity_id,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'stockholders_equity'
                      AND period_type = 'instant'
                ) AS equity_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'lt_debt'
                          AND period_type = 'instant'
                    ) AS lt_debt,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'lt_debt'
                          AND period_type = 'instant'
                    ) AS lt_debt_id,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'lt_debt'
                      AND period_type = 'instant'
                ) AS lt_debt_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'cash_st_inv'
                          AND period_type = 'instant'
                    ) AS cash_st_inv,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'cash_st_inv'
                          AND period_type = 'instant'
                    ) AS cash_st_inv_id,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'cash_st_inv'
                      AND period_type = 'instant'
                ) AS cash_st_inv_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric IN (
                    'revenue', 'stockholders_equity', 'lt_debt', 'cash_st_inv'
                  )
              AND unit = 'USD'
              AND period_end IS NOT NULL
              AND accession_number IS NOT NULL
              AND form IN (
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A'
              )
            GROUP BY security_id, accession_number, period_end
        ),
        revenue_events AS (
            SELECT *
            FROM annual_filings
            WHERE revenue > 0 AND isfinite(revenue)
        ),
        prior_complete AS (
            SELECT
                *,
                equity + lt_debt - cash_st_inv AS noa_proxy,
                greatest(
                    revenue_available_at,
                    equity_available_at,
                    lt_debt_available_at,
                    cash_st_inv_available_at
                ) AS annual_available_at
            FROM annual_filings
            WHERE revenue > 0
              AND equity IS NOT NULL
              AND lt_debt IS NOT NULL
              AND cash_st_inv IS NOT NULL
              AND isfinite(revenue)
              AND isfinite(equity)
              AND isfinite(lt_debt)
              AND isfinite(cash_st_inv)
              AND equity + lt_debt - cash_st_inv > 0
              AND isfinite(equity + lt_debt - cash_st_inv)
        ),
        noa_complete AS (
            SELECT
                *,
                equity + lt_debt - cash_st_inv AS noa_proxy,
                greatest(
                    equity_available_at,
                    lt_debt_available_at,
                    cash_st_inv_available_at
                ) AS noa_available_at
            FROM annual_filings
            WHERE equity IS NOT NULL
              AND lt_debt IS NOT NULL
              AND cash_st_inv IS NOT NULL
              AND isfinite(equity)
              AND isfinite(lt_debt)
              AND isfinite(cash_st_inv)
              AND equity + lt_debt - cash_st_inv > 0
              AND isfinite(equity + lt_debt - cash_st_inv)
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (SELECT DISTINCT security_id FROM revenue_events)
              AND close > 0
              AND trade_date IS NOT NULL
              AND available_at IS NOT NULL
            GROUP BY security_id, trade_date
        ),
        price_months AS (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY security_id, year(trade_date), month(trade_date)
                    ORDER BY trade_date DESC
                ) AS month_rank
            FROM price_dedup
        ),
        rebalances AS (
            SELECT * FROM price_months
            WHERE month_rank = 1 {date_sql}
        ),
        governed_rebalances AS (
            SELECT
                p.*,
                u.valid_from AS universe_valid_from,
                u.valid_to AS universe_valid_to,
                u.universe_id,
                u.available_at AS universe_available_at,
                u.source AS universe_source,
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY u.valid_from DESC,
                             u.available_at DESC NULLS LAST,
                             u.source_loaded_at DESC,
                             u.source DESC
                ) AS universe_rank
            FROM rebalances p
            JOIN universe_membership u
              ON u.universe_id = ?
             AND u.security_id = p.security_id
             AND u.valid_from <= p.trade_date
             AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
             AND u.as_of_date <= p.trade_date
             AND u.is_member
             AND u.is_latest_revision
             AND (u.available_at IS NULL OR u.available_at <= p.price_available_at)
        ),
        current_candidates AS (
            SELECT
                d.*,
                a.fundamental_symbol,
                a.accession_number AS current_accession_number,
                a.period_end AS current_period_end,
                a.revenue AS current_revenue,
                a.revenue_id AS current_revenue_id,
                a.revenue_available_at AS current_revenue_available_at,
                row_number() OVER (
                    PARTITION BY d.security_id, d.trade_date
                    ORDER BY a.period_end DESC,
                             a.revenue_available_at DESC,
                             a.accession_number DESC
                ) AS current_rank
            FROM governed_rebalances d
            JOIN revenue_events a
              ON a.security_id = d.security_id
             AND a.period_end <= d.trade_date
             AND a.revenue_available_at <= d.price_available_at
             AND d.trade_date - a.period_end <= ?
            WHERE d.universe_rank = 1
        ),
        current_annual AS (
            SELECT * EXCLUDE (current_rank)
            FROM current_candidates
            WHERE current_rank = 1
        ),
        prior_candidates AS (
            SELECT
                c.*,
                p.accession_number AS prior_accession_number,
                p.period_end AS prior_period_end,
                p.revenue AS prior_revenue,
                p.revenue_id AS prior_revenue_id,
                p.revenue_available_at AS prior_revenue_available_at,
                p.equity AS prior_equity,
                p.equity_id AS prior_equity_id,
                p.equity_available_at AS prior_equity_available_at,
                p.lt_debt AS prior_lt_debt,
                p.lt_debt_id AS prior_lt_debt_id,
                p.lt_debt_available_at AS prior_lt_debt_available_at,
                p.cash_st_inv AS prior_cash_st_inv,
                p.cash_st_inv_id AS prior_cash_st_inv_id,
                p.cash_st_inv_available_at AS prior_cash_st_inv_available_at,
                p.noa_proxy AS prior_noa_proxy,
                p.annual_available_at AS prior_annual_available_at,
                row_number() OVER (
                    PARTITION BY c.security_id, c.trade_date
                    ORDER BY abs((c.current_period_end - p.period_end) - 365),
                             p.period_end DESC,
                             p.annual_available_at DESC,
                             p.accession_number DESC
                ) AS prior_rank
            FROM current_annual c
            JOIN prior_complete p
              ON p.security_id = c.security_id
             AND p.period_end < c.current_period_end
             AND c.current_period_end - p.period_end BETWEEN 300 AND 430
             AND p.annual_available_at <= c.price_available_at
        ),
        current_prior AS (
            SELECT * EXCLUDE (prior_rank)
            FROM prior_candidates
            WHERE prior_rank = 1
        ),
        prior2_candidates AS (
            SELECT
                cp.*,
                a.accession_number AS prior2_accession_number,
                a.period_end AS prior2_period_end,
                a.equity AS prior2_equity,
                a.equity_id AS prior2_equity_id,
                a.equity_available_at AS prior2_equity_available_at,
                a.lt_debt AS prior2_lt_debt,
                a.lt_debt_id AS prior2_lt_debt_id,
                a.lt_debt_available_at AS prior2_lt_debt_available_at,
                a.cash_st_inv AS prior2_cash_st_inv,
                a.cash_st_inv_id AS prior2_cash_st_inv_id,
                a.cash_st_inv_available_at AS prior2_cash_st_inv_available_at,
                a.noa_proxy AS prior2_noa_proxy,
                a.noa_available_at AS prior2_noa_available_at,
                row_number() OVER (
                    PARTITION BY cp.security_id, cp.trade_date
                    ORDER BY abs((cp.prior_period_end - a.period_end) - 365),
                             a.period_end DESC,
                             a.noa_available_at DESC,
                             a.accession_number DESC
                ) AS prior2_rank
            FROM current_prior cp
            JOIN noa_complete a
              ON a.security_id = cp.security_id
             AND a.period_end < cp.prior_period_end
             AND cp.prior_period_end - a.period_end BETWEEN 300 AND 430
             AND a.noa_available_at <= cp.price_available_at
        )
        SELECT
            * EXCLUDE (month_rank, universe_rank, prior2_rank),
            greatest(
                price_available_at,
                universe_available_at,
                current_revenue_available_at,
                prior_annual_available_at,
                prior2_noa_available_at
            ) AS decision_available_at
        FROM prior2_candidates
        WHERE prior2_rank = 1
        ORDER BY trade_date, security_id
    """
    return store.con.execute(
        sql,
        [*date_params, options.universe_id, options.maximum_fundamental_age_days],
    ).df()


def _component(row: pd.Series, prefix: str, name: str) -> dict[str, object]:
    return {
        "statement_point_id": row[f"{prefix}_{name}_id"],
        "value": row[f"{prefix}_{name}"],
        "available_at": row[f"{prefix}_{name}_available_at"],
    }


def _lineage(row: pd.Series, options: NoaProxyTurnoverChangeOptions) -> str:
    return json_dumps(
        {
            "method": "annual_change_in_noa_proxy_turnover_pit",
            "formula": (
                "revenue_t/noa_proxy_t_1-revenue_t_1/noa_proxy_t_2;"
                "noa_proxy=stockholders_equity+lt_debt-cash_st_inv"
            ),
            "orientation": "higher_efficiency_change_is_preferred",
            "proxy_warning": (
                "Not exact net operating assets: broad PIT short-term debt, preferred "
                "stock, minority interest, operating liabilities, and all financial "
                "assets are unavailable."
            ),
            "research_contract": {
                "annual_duration_days_inclusive": [330, 400],
                "annual_period_gap_days": [300, 430],
                "maximum_fundamental_age_days": options.maximum_fundamental_age_days,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "missing_components_imputed": False,
                "return_fitted_parameters": False,
            },
            "decision": {
                "trade_date": row["trade_date"],
                "available_at": row["decision_available_at"],
                "universe_id": row["universe_id"],
                "universe_valid_from": row["universe_valid_from"],
                "universe_valid_to": row["universe_valid_to"],
                "universe_available_at": row["universe_available_at"],
                "universe_source": row["universe_source"],
            },
            "current": {
                "accession_number": row["current_accession_number"],
                "period_end": row["current_period_end"],
                "revenue": {
                    "statement_point_id": row["current_revenue_id"],
                    "value": row["current_revenue"],
                    "available_at": row["current_revenue_available_at"],
                },
            },
            "prior": {
                "accession_number": row["prior_accession_number"],
                "period_end": row["prior_period_end"],
                "revenue": {
                    "statement_point_id": row["prior_revenue_id"],
                    "value": row["prior_revenue"],
                    "available_at": row["prior_revenue_available_at"],
                },
                "stockholders_equity": _component(row, "prior", "equity"),
                "long_term_debt": _component(row, "prior", "lt_debt"),
                "cash_and_short_term_investments": _component(
                    row, "prior", "cash_st_inv"
                ),
                "noa_proxy": row["prior_noa_proxy"],
            },
            "prior2": {
                "accession_number": row["prior2_accession_number"],
                "period_end": row["prior2_period_end"],
                "stockholders_equity": _component(row, "prior2", "equity"),
                "long_term_debt": _component(row, "prior2", "lt_debt"),
                "cash_and_short_term_investments": _component(
                    row, "prior2", "cash_st_inv"
                ),
                "noa_proxy": row["prior2_noa_proxy"],
            },
            "asset_turnover": row["asset_turnover"],
            "prior_asset_turnover": row["prior_asset_turnover"],
            "change": row["raw_value"],
        }
    )


def compute_noa_proxy_turnover_change_rows(
    inputs: pd.DataFrame,
    options: NoaProxyTurnoverChangeOptions | None = None,
) -> pd.DataFrame:
    """Compute, winsorize, and standardize annual NOA-proxy turnover changes."""

    options = options or NoaProxyTurnoverChangeOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "decision_available_at",
        "current_revenue",
        "prior_revenue",
        "prior_noa_proxy",
        "prior2_noa_proxy",
        "current_revenue_id",
        "prior_revenue_id",
        "current_accession_number",
        "prior_accession_number",
        "prior2_accession_number",
        "current_period_end",
        "prior_period_end",
        "prior2_period_end",
        "current_revenue_available_at",
        "prior_revenue_available_at",
        "universe_id",
        "universe_valid_from",
        "universe_valid_to",
        "universe_available_at",
        "universe_source",
    }
    for prefix in ("prior", "prior2"):
        for component in ("equity", "lt_debt", "cash_st_inv"):
            required.update(
                {
                    f"{prefix}_{component}",
                    f"{prefix}_{component}_id",
                    f"{prefix}_{component}_available_at",
                }
            )
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"NOA-proxy turnover inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    numeric_columns = (
        "current_revenue",
        "prior_revenue",
        "prior_noa_proxy",
        "prior2_noa_proxy",
    )
    for column in numeric_columns:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "available_at", *numeric_columns]
    )
    valid = pd.Series(True, index=rows.index)
    for column in numeric_columns:
        valid &= (rows[column] > 0) & rows[column].map(math.isfinite)
    rows = rows[valid].copy()
    rows["asset_turnover"] = rows["current_revenue"] / rows["prior_noa_proxy"]
    rows["prior_asset_turnover"] = rows["prior_revenue"] / rows["prior2_noa_proxy"]
    rows["raw_value"] = rows["asset_turnover"] - rows["prior_asset_turnover"]
    rows = rows[rows["raw_value"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
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
    rows["input_ids_json"] = json_dumps(
        [
            "metric:revenue",
            "metric:stockholders_equity",
            "metric:lt_debt",
            "metric:cash_st_inv",
            f"universe:{options.universe_id}",
        ]
    )
    rows["input_lineage_json"] = rows.apply(
        lambda row: _lineage(row, options), axis=1
    )
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, security_id, as_of_date)
        for security_id, as_of_date in zip(
            rows["security_id"], rows["as_of_date"], strict=True
        )
    ]
    return (
        rows[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_noa_proxy_turnover_change_values(
    store: DuckDBStore,
    options: NoaProxyTurnoverChangeOptions | None = None,
) -> int:
    """Materialize the annual NOA-proxy turnover-change feature."""

    options = options or NoaProxyTurnoverChangeOptions()
    store.initialize()
    rows = compute_noa_proxy_turnover_change_rows(
        load_noa_proxy_turnover_change_inputs(store, options), options
    )
    predicates = ["source = ?", "factor_id = ?"]
    params: list[object] = [options.source, FACTOR_ID]
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
                "noa_proxy_turnover_change_insert",
            )
    return len(rows)
