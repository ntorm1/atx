"""Point-in-time corrected public-company Altman Z-score.

The implementation uses total liabilities in the market-equity leg, correcting
the legacy empty definition that depended on an unpopulated total-debt metric.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .cash_flow_profitability import FACTOR_IDS as CASH_FACTOR_IDS
from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .fundamental_signals import FACTOR_IDS as FUNDAMENTAL_SIGNAL_IDS
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT corrected Altman Z-score v1"
FACTOR_ID = "distress_altman_z_score"
FACTOR_NAME = "PIT corrected Altman Z-score"
FACTOR_FAMILY = "fundamental_distress"
CASH_SCAFFOLD_FACTOR_ID = CASH_FACTOR_IDS[0]
BOOK_TO_MARKET_FACTOR_ID = "value_book_to_market"

if BOOK_TO_MARKET_FACTOR_ID not in FUNDAMENTAL_SIGNAL_IDS:
    raise RuntimeError("book-to-market factor identifier drifted")

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
class AltmanDistressOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    maximum_absolute_z_score: float = 100.0
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_altman_distress_inputs(
    store: DuckDBStore,
    options: AltmanDistressOptions | None = None,
) -> pd.DataFrame:
    """Resolve corrected Altman inputs visible on governed monthly factor keys."""

    options = options or AltmanDistressOptions()
    predicates = ["c.factor_id = ?", "b.factor_id = ?", "c.is_latest_revision", "b.is_latest_revision"]
    params: list[object] = [CASH_SCAFFOLD_FACTOR_ID, BOOK_TO_MARKET_FACTOR_ID]
    if options.start_date is not None:
        predicates.append("c.as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("c.as_of_date <= ?")
        params.append(options.end_date)
    sql = f"""
        WITH decisions AS (
            SELECT
                c.security_id,
                c.symbol,
                c.as_of_date,
                greatest(c.available_at, b.available_at) AS decision_available_at,
                c.factor_value_id AS cash_scaffold_factor_value_id,
                b.factor_value_id AS book_to_market_factor_value_id,
                CAST(
                    json_extract_string(c.input_lineage_json, '$.ttm.period_end')
                    AS TIMESTAMP
                )::DATE AS ttm_end_date,
                CAST(
                    json_extract_string(c.input_lineage_json, '$.assets.current_period_end')
                    AS TIMESTAMP
                )::DATE AS asset_period_end,
                CAST(
                    json_extract_string(c.input_lineage_json, '$.assets.current_value')
                    AS DOUBLE
                ) AS assets,
                CAST(
                    json_extract_string(b.input_lineage_json, '$.market_cap_usd')
                    AS DOUBLE
                ) AS market_cap_usd,
                row_number() OVER (
                    PARTITION BY c.security_id, c.as_of_date
                    ORDER BY c.available_at DESC, c.source_loaded_at DESC,
                             b.available_at DESC, b.source_loaded_at DESC
                ) AS decision_rank
            FROM fundamental_factor_values c
            JOIN fundamental_factor_values b USING (security_id, as_of_date)
            WHERE {" AND ".join(predicates)}
              AND c.value IS NOT NULL AND isfinite(c.value)
              AND b.value IS NOT NULL AND isfinite(b.value)
        ),
        statement_inputs AS (
            SELECT
                d.*,
                arg_max(p.value, (p.available_at, p.revision_sequence, p.statement_point_id))
                    FILTER (WHERE p.canonical_metric = 'current_assets') AS current_assets,
                arg_max(
                    p.statement_point_id,
                    (p.available_at, p.revision_sequence, p.statement_point_id)
                ) FILTER (WHERE p.canonical_metric = 'current_assets') AS current_assets_id,
                max(p.available_at) FILTER (WHERE p.canonical_metric = 'current_assets')
                    AS current_assets_available_at,
                arg_max(p.value, (p.available_at, p.revision_sequence, p.statement_point_id))
                    FILTER (WHERE p.canonical_metric = 'current_liabilities')
                    AS current_liabilities,
                arg_max(
                    p.statement_point_id,
                    (p.available_at, p.revision_sequence, p.statement_point_id)
                ) FILTER (WHERE p.canonical_metric = 'current_liabilities')
                    AS current_liabilities_id,
                max(p.available_at) FILTER (WHERE p.canonical_metric = 'current_liabilities')
                    AS current_liabilities_available_at,
                arg_max(p.value, (p.available_at, p.revision_sequence, p.statement_point_id))
                    FILTER (WHERE p.canonical_metric = 'retained_earnings')
                    AS retained_earnings,
                arg_max(
                    p.statement_point_id,
                    (p.available_at, p.revision_sequence, p.statement_point_id)
                ) FILTER (WHERE p.canonical_metric = 'retained_earnings')
                    AS retained_earnings_id,
                max(p.available_at) FILTER (WHERE p.canonical_metric = 'retained_earnings')
                    AS retained_earnings_available_at,
                arg_max(p.value, (p.available_at, p.revision_sequence, p.statement_point_id))
                    FILTER (WHERE p.canonical_metric = 'total_liabilities')
                    AS total_liabilities,
                arg_max(
                    p.statement_point_id,
                    (p.available_at, p.revision_sequence, p.statement_point_id)
                ) FILTER (WHERE p.canonical_metric = 'total_liabilities')
                    AS total_liabilities_id,
                max(p.available_at) FILTER (WHERE p.canonical_metric = 'total_liabilities')
                    AS total_liabilities_available_at
            FROM decisions d
            JOIN fundamental_statement_points p
              ON p.security_id = d.security_id
             AND p.period_end = d.asset_period_end
             AND p.canonical_metric IN (
                 'current_assets', 'current_liabilities', 'retained_earnings',
                 'total_liabilities'
             )
             AND p.unit = 'USD'
             AND p.available_at <= d.decision_available_at
             AND p.value IS NOT NULL
             AND isfinite(p.value)
            WHERE d.decision_rank = 1
            GROUP BY ALL
        ),
        ttm_inputs AS (
            SELECT
                s.*,
                arg_max(t.ttm_value, (t.available_at, t.revision_sequence, t.ttm_point_id))
                    FILTER (WHERE t.canonical_metric = 'operating_income') AS ebit_ttm,
                arg_max(
                    t.ttm_point_id,
                    (t.available_at, t.revision_sequence, t.ttm_point_id)
                ) FILTER (WHERE t.canonical_metric = 'operating_income') AS ebit_ttm_id,
                max(t.available_at) FILTER (WHERE t.canonical_metric = 'operating_income')
                    AS ebit_ttm_available_at,
                arg_max(t.ttm_value, (t.available_at, t.revision_sequence, t.ttm_point_id))
                    FILTER (WHERE t.canonical_metric = 'revenue') AS revenue_ttm,
                arg_max(
                    t.ttm_point_id,
                    (t.available_at, t.revision_sequence, t.ttm_point_id)
                ) FILTER (WHERE t.canonical_metric = 'revenue') AS revenue_ttm_id,
                max(t.available_at) FILTER (WHERE t.canonical_metric = 'revenue')
                    AS revenue_ttm_available_at
            FROM statement_inputs s
            JOIN fundamental_ttm_points t
              ON t.security_id = s.security_id
             AND t.ttm_end_date = s.ttm_end_date
             AND t.canonical_metric IN ('operating_income', 'revenue')
             AND t.unit = 'USD'
             AND t.available_at <= s.decision_available_at
             AND t.ttm_value IS NOT NULL
             AND isfinite(t.ttm_value)
            GROUP BY ALL
        )
        SELECT
            *,
            1.2 * (current_assets - current_liabilities) / assets
              + 1.4 * retained_earnings / assets
              + 3.3 * ebit_ttm / assets
              + 0.6 * market_cap_usd / total_liabilities
              + revenue_ttm / assets AS altman_z_score
        FROM ttm_inputs
        WHERE assets > 0
          AND total_liabilities > 0
          AND market_cap_usd > 0
          AND current_assets IS NOT NULL
          AND current_liabilities IS NOT NULL
          AND retained_earnings IS NOT NULL
          AND ebit_ttm IS NOT NULL
          AND revenue_ttm IS NOT NULL
        ORDER BY as_of_date, security_id
    """
    rows = store.con.execute(sql, params).df()
    if rows.empty:
        return rows
    rows["altman_z_score"] = pd.to_numeric(rows["altman_z_score"], errors="coerce")
    return rows[
        rows["altman_z_score"].map(math.isfinite) & (rows["altman_z_score"].abs() <= options.maximum_absolute_z_score)
    ].reset_index(drop=True)


def _lineage(row: pd.Series) -> str:
    def metric(prefix: str, value_column: str) -> dict[str, object]:
        return {
            "id": row[f"{prefix}_id"],
            "value": row[value_column],
            "available_at": row[f"{prefix}_available_at"],
        }

    return json_dumps(
        {
            "method": "corrected_public_company_altman_z",
            "formula": "1.2*(current_assets-current_liabilities)/assets + 1.4*retained_earnings/assets + 3.3*operating_income_ttm/assets + 0.6*market_cap/total_liabilities + revenue_ttm/assets",
            "correction": "Uses total liabilities, not the legacy empty total_debt dependency, in the market-equity leg.",
            "decision": {
                "as_of_date": row["as_of_date"],
                "available_at": row["decision_available_at"],
                "cash_scaffold_factor_value_id": row["cash_scaffold_factor_value_id"],
                "book_to_market_factor_value_id": row["book_to_market_factor_value_id"],
            },
            "periods": {
                "ttm_end_date": row["ttm_end_date"],
                "asset_period_end": row["asset_period_end"],
            },
            "inputs": {
                "assets": row["assets"],
                "market_cap_usd": row["market_cap_usd"],
                "current_assets": metric("current_assets", "current_assets"),
                "current_liabilities": metric("current_liabilities", "current_liabilities"),
                "retained_earnings": metric("retained_earnings", "retained_earnings"),
                "total_liabilities": metric("total_liabilities", "total_liabilities"),
                "operating_income_ttm": metric("ebit_ttm", "ebit_ttm"),
                "revenue_ttm": metric("revenue_ttm", "revenue_ttm"),
            },
        }
    )


def compute_altman_distress_rows(
    inputs: pd.DataFrame,
    options: AltmanDistressOptions | None = None,
) -> pd.DataFrame:
    """Winsorize and standardize corrected Altman scores cross-sectionally."""

    options = options or AltmanDistressOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {"security_id", "symbol", "as_of_date", "decision_available_at", "altman_z_score"}
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"Altman distress inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    rows["altman_z_score"] = pd.to_numeric(rows["altman_z_score"], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", "altman_z_score"])
    rows = rows[rows["altman_z_score"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["altman_z_score"].astype(float)
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
            f"factor:{CASH_SCAFFOLD_FACTOR_ID}",
            f"factor:{BOOK_TO_MARKET_FACTOR_ID}",
            "metric:current_assets",
            "metric:current_liabilities",
            "metric:retained_earnings",
            "metric:total_liabilities",
            "metric:operating_income_ttm",
            "metric:revenue_ttm",
        ]
    )
    rows["input_lineage_json"] = rows.apply(_lineage, axis=1)
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, security_id, as_of_date)
        for security_id, as_of_date in zip(rows["security_id"], rows["as_of_date"], strict=True)
    ]
    return (
        rows[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_altman_distress_values(
    store: DuckDBStore,
    options: AltmanDistressOptions | None = None,
) -> int:
    """Materialize the corrected PIT Altman Z-score."""

    options = options or AltmanDistressOptions()
    store.initialize()
    rows = compute_altman_distress_rows(load_altman_distress_inputs(store, options), options)
    predicates = ["source = ?", "factor_id = ?"]
    params: list[object] = [options.source, FACTOR_ID]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    with store.transaction():
        store.con.execute(f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}", params)
        if not rows.empty:
            insert_frame(store, rows, "fundamental_factor_values", "altman_distress_insert")
    return len(rows)
