"""Point-in-time rank-standardized QMJ profitability composite."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import zscore
from .piotroski import FACTOR_ID as PIOTROSKI_FACTOR_ID
from .piotroski import SOURCE_NAME as PIOTROSKI_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT QMJ profitability v1"
FACTOR_ID = "quality_qmj_profitability"
FACTOR_NAME = "PIT QMJ profitability"
FACTOR_FAMILY = "fundamental_quality"
BOOK_TO_MARKET_FACTOR_ID = "value_book_to_market"
BOOK_TO_MARKET_SOURCE = "atx-db PIT fundamental signals v1"

COMPONENT_COLUMNS = (
    "gross_profit_to_assets",
    "return_on_equity",
    "return_on_assets",
    "operating_cash_flow_to_assets",
    "gross_margin",
    "low_accruals",
)
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
class QmjProfitabilityOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_names_per_date: int = 20
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_qmj_profitability_inputs(
    store: DuckDBStore,
    options: QmjProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Read six exact-period profitability inputs from governed factor lineage."""

    options = options or QmjProfitabilityOptions()
    date_predicates: list[str] = []
    date_params: list[object] = []
    if options.start_date is not None:
        date_predicates.append("p.as_of_date >= ?")
        date_params.append(options.start_date)
    if options.end_date is not None:
        date_predicates.append("p.as_of_date <= ?")
        date_params.append(options.end_date)
    date_sql = " AND " + " AND ".join(date_predicates) if date_predicates else ""
    sql = f"""
        WITH piotroski AS (
            SELECT
                p.factor_value_id AS piotroski_factor_value_id,
                p.security_id,
                p.symbol,
                p.as_of_date,
                p.available_at,
                CAST(
                    json_extract_string(p.input_lineage_json, '$.periods.current')
                    AS DATE
                ) AS current_period_end,
                json_extract_string(
                    p.input_lineage_json,
                    '$.decision.book_to_market_factor_value_id'
                ) AS book_to_market_factor_value_id,
                CAST(
                    json_extract_string(
                        p.input_lineage_json,
                        '$.inputs.current.net_income.value'
                    ) AS DOUBLE
                ) AS net_income,
                CAST(
                    json_extract_string(
                        p.input_lineage_json,
                        '$.inputs.current.operating_cash_flow.value'
                    ) AS DOUBLE
                ) AS operating_cash_flow,
                CAST(
                    json_extract_string(
                        p.input_lineage_json,
                        '$.inputs.current.gross_profit.value'
                    ) AS DOUBLE
                ) AS gross_profit,
                CAST(
                    json_extract_string(
                        p.input_lineage_json,
                        '$.inputs.current.revenue.value'
                    ) AS DOUBLE
                ) AS revenue,
                CAST(
                    json_extract_string(
                        p.input_lineage_json,
                        '$.inputs.current.total_assets.value'
                    ) AS DOUBLE
                ) AS total_assets
            FROM fundamental_factor_values p
            WHERE p.factor_id = ?
              AND p.source = ?
              AND p.is_latest_revision
              AND p.value IS NOT NULL
              AND isfinite(p.value)
              {date_sql}
        ),
        book_equity AS (
            SELECT
                p.*,
                b.available_at AS book_to_market_available_at,
                json_extract_string(
                    b.input_lineage_json,
                    '$.stockholders_equity.id'
                ) AS book_equity_statement_point_id,
                CAST(
                    json_extract_string(
                        b.input_lineage_json,
                        '$.stockholders_equity.value'
                    ) AS DOUBLE
                ) AS book_equity
            FROM piotroski p
            JOIN fundamental_factor_values b
              ON b.factor_value_id = p.book_to_market_factor_value_id
             AND b.factor_id = ?
             AND b.source = ?
             AND b.is_latest_revision
        )
        SELECT
            b.*,
            s.period_end AS book_equity_period_end,
            s.available_at AS book_equity_available_at
        FROM book_equity b
        JOIN fundamental_statement_points s
          ON s.statement_point_id = b.book_equity_statement_point_id
         AND s.period_end = b.current_period_end
         AND s.available_at <= b.available_at
        WHERE b.total_assets > 0
          AND b.book_equity > 0
          AND b.revenue > 0
          AND isfinite(b.net_income)
          AND isfinite(b.operating_cash_flow)
          AND isfinite(b.gross_profit)
          AND isfinite(b.total_assets)
          AND isfinite(b.book_equity)
        ORDER BY b.as_of_date, b.security_id
    """
    return store.con.execute(
        sql,
        [
            PIOTROSKI_FACTOR_ID,
            PIOTROSKI_SOURCE_NAME,
            *date_params,
            BOOK_TO_MARKET_FACTOR_ID,
            BOOK_TO_MARKET_SOURCE,
        ],
    ).df()


def _rank_zscore(values: pd.Series, dates: pd.Series) -> pd.Series:
    ranks = values.groupby(dates).rank(method="average", na_option="keep")
    means = ranks.groupby(dates).transform("mean")
    standard_deviations = ranks.groupby(dates).transform(lambda group: group.std(ddof=1))
    result = (ranks - means) / standard_deviations
    return result.where(result.map(lambda value: pd.isna(value) or math.isfinite(value)))


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "method": "qmj_profitability_rank_composite",
            "research_contract": {
                "ranking": "average cross-sectional rank, sample z-score by date",
                "aggregation": "equal-weight mean of six component z-scores",
                "reported_cash_flow_adaptation": (
                    "Uses reported operating cash flow/assets instead of the QMJ synthetic "
                    "cash-flow proxy."
                ),
                "return_fitted_weights": False,
                "complete_case": True,
            },
            "components": {
                component: {
                    "raw": row[component],
                    "rank_z": row[f"{component}_rank_z"],
                }
                for component in COMPONENT_COLUMNS
            },
            "upstream": {
                "piotroski_factor_id": PIOTROSKI_FACTOR_ID,
                "piotroski_factor_value_id": row["piotroski_factor_value_id"],
                "book_to_market_factor_id": BOOK_TO_MARKET_FACTOR_ID,
                "book_to_market_factor_value_id": row[
                    "book_to_market_factor_value_id"
                ],
                "available_at": row["available_at"],
            },
            "book_equity": {
                "statement_point_id": row["book_equity_statement_point_id"],
                "value": row["book_equity"],
                "period_end": row["book_equity_period_end"],
                "available_at": row["book_equity_available_at"],
            },
        }
    )


def compute_qmj_profitability_rows(
    inputs: pd.DataFrame,
    options: QmjProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Rank-standardize and equal-weight the six QMJ profitability dimensions."""

    options = options or QmjProfitabilityOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "as_of_date",
        "available_at",
        "net_income",
        "operating_cash_flow",
        "gross_profit",
        "revenue",
        "total_assets",
        "book_equity",
        "piotroski_factor_value_id",
        "book_to_market_factor_value_id",
        "book_equity_statement_point_id",
        "book_equity_period_end",
        "book_equity_available_at",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"QMJ profitability inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["available_at"], errors="coerce")
    numeric = [
        "net_income",
        "operating_cash_flow",
        "gross_profit",
        "revenue",
        "total_assets",
        "book_equity",
    ]
    for column in numeric:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", *numeric])
    rows = rows[
        (rows["total_assets"] > 0)
        & (rows["book_equity"] > 0)
        & (rows["revenue"] > 0)
    ].copy()
    rows["gross_profit_to_assets"] = rows["gross_profit"] / rows["total_assets"]
    rows["return_on_equity"] = rows["net_income"] / rows["book_equity"]
    rows["return_on_assets"] = rows["net_income"] / rows["total_assets"]
    rows["operating_cash_flow_to_assets"] = (
        rows["operating_cash_flow"] / rows["total_assets"]
    )
    rows["gross_margin"] = rows["gross_profit"] / rows["revenue"]
    rows["low_accruals"] = (
        rows["operating_cash_flow"] - rows["net_income"]
    ) / rows["total_assets"]
    component_z: list[str] = []
    for component in COMPONENT_COLUMNS:
        standardized = f"{component}_rank_z"
        rows[standardized] = _rank_zscore(rows[component], rows["as_of_date"])
        component_z.append(standardized)
    rows["qmj_profitability"] = rows[component_z].mean(axis=1)
    rows = rows[rows["qmj_profitability"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["qmj_profitability"].astype(float)
    rows = zscore(
        rows,
        value_column="raw_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        [f"factor:{PIOTROSKI_FACTOR_ID}", f"factor:{BOOK_TO_MARKET_FACTOR_ID}"]
    )
    rows["input_lineage_json"] = rows.apply(_lineage, axis=1)
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


def refresh_qmj_profitability_values(
    store: DuckDBStore,
    options: QmjProfitabilityOptions | None = None,
) -> int:
    """Materialize the complete-case PIT QMJ profitability composite."""

    options = options or QmjProfitabilityOptions()
    store.initialize()
    rows = compute_qmj_profitability_rows(load_qmj_profitability_inputs(store, options), options)
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
            insert_frame(store, rows, "fundamental_factor_values", "qmj_profitability_insert")
    return len(rows)
