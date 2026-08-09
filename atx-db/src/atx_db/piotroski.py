"""Point-in-time annual Piotroski financial-strength factors.

The original F-score is a nine-signal annual accounting score intended to
separate strong from weak firms inside the high book-to-market portfolio.  This
module exposes both the complete-case score and that paper-faithful conditional
feature.  Annual statement revisions must be visible at the monthly decision;
missing inputs are never imputed.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT Piotroski F-score v1"
FACTOR_ID = "quality_piotroski_f_score"
HIGH_BOOK_TO_MARKET_FACTOR_ID = "quality_piotroski_high_book_to_market"
FACTOR_IDS = (FACTOR_ID, HIGH_BOOK_TO_MARKET_FACTOR_ID)
FACTOR_METADATA = {
    FACTOR_ID: {
        "factor_name": "PIT Piotroski F-score",
        "family": "fundamental_quality",
    },
    HIGH_BOOK_TO_MARKET_FACTOR_ID: {
        "factor_name": "PIT Piotroski F-score within high book-to-market",
        "family": "fundamental_quality",
    },
}

BOOK_TO_MARKET_FACTOR_ID = "value_book_to_market"
BOOK_TO_MARKET_SOURCE = "atx-db PIT fundamental signals v1"
NET_ISSUANCE_FACTOR_ID = "financing_low_net_share_issuance"
NET_ISSUANCE_SOURCE = "atx-db PIT net share issuance v1"

_ANNUAL_METRICS = (
    "net_income",
    "operating_cash_flow",
    "revenue",
    "gross_profit",
    "total_assets",
    "current_assets",
    "current_liabilities",
    "lt_debt",
)
_FLOW_METRICS = {
    "net_income",
    "operating_cash_flow",
    "revenue",
    "gross_profit",
}
_SIGNAL_COLUMNS = (
    "positive_roa",
    "positive_cfo",
    "improving_roa",
    "low_accruals",
    "falling_leverage",
    "improving_liquidity",
    "no_net_share_issuance",
    "improving_gross_margin",
    "improving_asset_turnover",
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
class PiotroskiOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_fundamental_age_days: int = 550
    high_book_to_market_percentile: float = 0.80
    minimum_names_per_date: int = 20
    minimum_high_book_to_market_names_per_date: int = 10
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(
    source: str,
    factor_id: str,
    security_id: str,
    as_of_date: Any,
) -> str:
    payload = "|".join(str(part) for part in (source, factor_id, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _date_filter(options: PiotroskiOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def _annual_pivot_columns() -> str:
    columns: list[str] = []
    for metric in _ANNUAL_METRICS:
        period_filter = (
            " AND period_type = 'duration'"
            " AND period_start IS NOT NULL"
            " AND period_end - period_start BETWEEN 329 AND 379"
            if metric in _FLOW_METRICS
            else ""
        )
        predicate = f"canonical_metric = '{metric}'{period_filter}"
        order = "(available_at, revision_sequence, statement_point_id)"
        columns.extend(
            [
                f"arg_max(value, {order}) FILTER (WHERE {predicate}) AS {metric}",
                f"arg_max(statement_point_id, {order}) "
                f"FILTER (WHERE {predicate}) AS {metric}_id",
                f"max(available_at) FILTER (WHERE {predicate}) "
                f"AS {metric}_available_at",
            ]
        )
    return ",\n                ".join(columns)


def _prefixed_columns(prefix: str, source_prefix: str, metrics: tuple[str, ...]) -> str:
    columns: list[str] = []
    for metric in metrics:
        columns.extend(
            [
                f"{source_prefix}.{metric} AS {prefix}_{metric}",
                f"{source_prefix}.{metric}_id AS {prefix}_{metric}_id",
                f"{source_prefix}.{metric}_available_at "
                f"AS {prefix}_{metric}_available_at",
            ]
        )
    return ",\n                ".join(columns)


def load_piotroski_inputs(
    store: DuckDBStore,
    options: PiotroskiOptions | None = None,
) -> pd.DataFrame:
    """Assemble exact annual F-score inputs visible at governed monthly decisions."""

    options = options or PiotroskiOptions()
    date_sql, date_params = _date_filter(options)
    metric_filter = ", ".join(f"'{metric}'" for metric in _ANNUAL_METRICS)
    pivot_columns = _annual_pivot_columns()
    current_columns = _prefixed_columns("current", "c", _ANNUAL_METRICS)
    prior_columns = _prefixed_columns("prior", "p", _ANNUAL_METRICS)
    required = " AND ".join(
        [
            "net_income IS NOT NULL",
            "operating_cash_flow IS NOT NULL",
            "revenue > 0",
            "gross_profit IS NOT NULL",
            "total_assets > 0",
            "current_assets IS NOT NULL",
            "current_liabilities > 0",
            "lt_debt IS NOT NULL",
        ]
    )
    sql = f"""
        WITH annual_facts AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                period_end,
                {pivot_columns}
            FROM fundamental_statement_points
            WHERE canonical_metric IN ({metric_filter})
              AND unit = 'USD'
              AND period_end IS NOT NULL
              AND accession_number IS NOT NULL
              AND form IN (
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A'
              )
            GROUP BY security_id, accession_number, period_end
        ),
        annual AS (
            SELECT
                *,
                greatest(
                    net_income_available_at,
                    operating_cash_flow_available_at,
                    revenue_available_at,
                    gross_profit_available_at,
                    total_assets_available_at,
                    current_assets_available_at,
                    current_liabilities_available_at,
                    lt_debt_available_at
                ) AS annual_available_at
            FROM annual_facts
        ),
        complete_annual AS (
            SELECT *
            FROM annual
            WHERE {required}
              AND isfinite(net_income)
              AND isfinite(operating_cash_flow)
              AND isfinite(revenue)
              AND isfinite(gross_profit)
              AND isfinite(total_assets)
              AND isfinite(current_assets)
              AND isfinite(current_liabilities)
              AND isfinite(lt_debt)
        ),
        current_prior_candidates AS (
            SELECT
                c.security_id,
                c.fundamental_symbol,
                c.accession_number AS current_accession_number,
                c.period_end AS current_period_end,
                c.annual_available_at AS signal_available_at,
                {current_columns},
                p.accession_number AS prior_accession_number,
                p.period_end AS prior_period_end,
                p.annual_available_at AS prior_annual_available_at,
                {prior_columns},
                row_number() OVER (
                    PARTITION BY c.security_id, c.accession_number, c.period_end
                    ORDER BY
                        abs((c.period_end - p.period_end) - 365),
                        p.period_end DESC,
                        p.annual_available_at DESC,
                        p.accession_number DESC
                ) AS prior_rank
            FROM complete_annual c
            JOIN complete_annual p
              ON p.security_id = c.security_id
             AND p.period_end < c.period_end
             AND c.period_end - p.period_end BETWEEN 300 AND 430
             AND p.annual_available_at <= c.annual_available_at
        ),
        current_prior AS (
            SELECT * EXCLUDE (prior_rank)
            FROM current_prior_candidates
            WHERE prior_rank = 1
        ),
        prior2_candidates AS (
            SELECT
                cp.*,
                a.period_end AS prior2_period_end,
                a.total_assets AS prior2_total_assets,
                a.total_assets_id AS prior2_total_assets_id,
                a.total_assets_available_at AS prior2_total_assets_available_at,
                row_number() OVER (
                    PARTITION BY cp.security_id, cp.current_accession_number,
                                 cp.current_period_end
                    ORDER BY
                        abs((cp.prior_period_end - a.period_end) - 365),
                        a.period_end DESC,
                        a.total_assets_available_at DESC,
                        a.accession_number DESC
                ) AS prior2_rank
            FROM current_prior cp
            JOIN annual a
              ON a.security_id = cp.security_id
             AND a.period_end < cp.prior_period_end
             AND cp.prior_period_end - a.period_end BETWEEN 300 AND 430
             AND a.total_assets > 0
             AND isfinite(a.total_assets)
             AND a.total_assets_available_at <= cp.signal_available_at
        ),
        signal_events AS (
            SELECT * EXCLUDE (prior2_rank)
            FROM prior2_candidates
            WHERE prior2_rank = 1
        ),
        book_source_rows AS (
            SELECT
                factor_value_id AS book_to_market_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                raw_value AS book_to_market,
                value AS book_to_market_value,
                available_at AS book_to_market_available_at,
                source_loaded_at,
                row_number() OVER (
                    PARTITION BY security_id, as_of_date
                    ORDER BY available_at DESC, source_loaded_at DESC,
                             factor_value_id DESC
                ) AS source_rank
            FROM fundamental_factor_values
            WHERE factor_id = ?
              AND source = ?
              AND is_latest_revision
              AND raw_value IS NOT NULL
              AND isfinite(raw_value)
              {date_sql}
        ),
        book_ranked AS (
            SELECT
                * EXCLUDE (source_rank),
                percent_rank() OVER (
                    PARTITION BY as_of_date ORDER BY book_to_market
                ) AS book_to_market_percentile
            FROM book_source_rows
            WHERE source_rank = 1
        ),
        issuance_rows AS (
            SELECT
                factor_value_id AS net_issuance_factor_value_id,
                security_id,
                as_of_date,
                raw_value AS low_net_issuance,
                available_at AS net_issuance_available_at,
                row_number() OVER (
                    PARTITION BY security_id, as_of_date
                    ORDER BY available_at DESC, source_loaded_at DESC,
                             factor_value_id DESC
                ) AS issuance_rank
            FROM fundamental_factor_values
            WHERE factor_id = ?
              AND source = ?
              AND is_latest_revision
              AND raw_value IS NOT NULL
              AND isfinite(raw_value)
              {date_sql}
        ),
        decisions AS (
            SELECT
                b.*,
                i.net_issuance_factor_value_id,
                i.low_net_issuance,
                i.net_issuance_available_at,
                greatest(
                    b.book_to_market_available_at,
                    i.net_issuance_available_at
                ) AS decision_available_at
            FROM book_ranked b
            JOIN issuance_rows i USING (security_id, as_of_date)
            WHERE i.issuance_rank = 1
        ),
        matched AS (
            SELECT
                d.*,
                s.* EXCLUDE (security_id),
                row_number() OVER (
                    PARTITION BY d.security_id, d.as_of_date
                    ORDER BY s.current_period_end DESC,
                             s.signal_available_at DESC,
                             s.current_accession_number DESC
                ) AS signal_rank
            FROM decisions d
            JOIN signal_events s
              ON s.security_id = d.security_id
             AND s.current_period_end <= d.as_of_date
             AND s.signal_available_at <= d.decision_available_at
             AND d.as_of_date - s.current_period_end <= ?
        )
        SELECT * EXCLUDE (source_loaded_at, signal_rank)
        FROM matched
        WHERE signal_rank = 1
        ORDER BY as_of_date, security_id
    """
    params: list[object] = [
        BOOK_TO_MARKET_FACTOR_ID,
        BOOK_TO_MARKET_SOURCE,
        *date_params,
        NET_ISSUANCE_FACTOR_ID,
        NET_ISSUANCE_SOURCE,
        *date_params,
        options.maximum_fundamental_age_days,
    ]
    return store.con.execute(sql, params).df()


def _metric_lineage(row: pd.Series, prefix: str, metric: str) -> dict[str, object]:
    return {
        "id": row[f"{prefix}_{metric}_id"],
        "value": row[f"{prefix}_{metric}"],
        "available_at": row[f"{prefix}_{metric}_available_at"],
    }


def piotroski_input_lineage(
    row: pd.Series,
    options: PiotroskiOptions,
    factor_id: str,
) -> str:
    return json_dumps(
        {
            "method": "original_annual_piotroski_f_score_pit",
            "factor_id": factor_id,
            "formula": "sum(nine binary financial-strength signals)",
            "equity_offering_proxy": (
                "No net split-adjusted share issuance over the prior year; "
                "repurchases and unchanged shares score one."
            ),
            "decision": {
                "as_of_date": row["as_of_date"],
                "available_at": row["decision_available_at"],
                "book_to_market_factor_value_id": row[
                    "book_to_market_factor_value_id"
                ],
                "net_issuance_factor_value_id": row[
                    "net_issuance_factor_value_id"
                ],
                "book_to_market": row["book_to_market"],
                "book_to_market_percentile": row["book_to_market_percentile"],
                "high_book_to_market_cutoff": options.high_book_to_market_percentile,
                "is_high_book_to_market": row["is_high_book_to_market"],
            },
            "periods": {
                "current": row["current_period_end"],
                "prior": row["prior_period_end"],
                "prior2": row["prior2_period_end"],
                "current_accession": row["current_accession_number"],
                "prior_accession": row["prior_accession_number"],
            },
            "signals": {
                signal: bool(row[signal]) for signal in _SIGNAL_COLUMNS
            },
            "ratios": {
                "roa": row["roa"],
                "prior_roa": row["prior_roa"],
                "cfo_to_beginning_assets": row["cfo_to_beginning_assets"],
                "leverage": row["leverage"],
                "prior_leverage": row["prior_leverage"],
                "current_ratio": row["current_ratio"],
                "prior_current_ratio": row["prior_current_ratio"],
                "gross_margin": row["gross_margin"],
                "prior_gross_margin": row["prior_gross_margin"],
                "asset_turnover": row["asset_turnover"],
                "prior_asset_turnover": row["prior_asset_turnover"],
                "low_net_issuance": row["low_net_issuance"],
            },
            "inputs": {
                "current": {
                    metric: _metric_lineage(row, "current", metric)
                    for metric in _ANNUAL_METRICS
                },
                "prior": {
                    metric: _metric_lineage(row, "prior", metric)
                    for metric in _ANNUAL_METRICS
                },
                "prior2_total_assets": {
                    "id": row["prior2_total_assets_id"],
                    "value": row["prior2_total_assets"],
                    "available_at": row["prior2_total_assets_available_at"],
                },
            },
        }
    )


def calculate_piotroski_components(
    inputs: pd.DataFrame,
    options: PiotroskiOptions,
) -> pd.DataFrame:
    rows = inputs.copy()
    numeric = [
        *(f"current_{metric}" for metric in _ANNUAL_METRICS),
        *(f"prior_{metric}" for metric in _ANNUAL_METRICS),
        "prior2_total_assets",
        "low_net_issuance",
        "book_to_market",
        "book_to_market_percentile",
    ]
    for column in numeric:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(subset=numeric).copy()
    positive_denominators = (
        (rows["current_total_assets"] > 0)
        & (rows["prior_total_assets"] > 0)
        & (rows["prior2_total_assets"] > 0)
        & (rows["current_current_liabilities"] > 0)
        & (rows["prior_current_liabilities"] > 0)
        & (rows["current_revenue"] > 0)
        & (rows["prior_revenue"] > 0)
    )
    rows = rows[positive_denominators].copy()
    if rows.empty:
        return rows

    rows["roa"] = rows["current_net_income"] / rows["prior_total_assets"]
    rows["prior_roa"] = rows["prior_net_income"] / rows["prior2_total_assets"]
    rows["cfo_to_beginning_assets"] = (
        rows["current_operating_cash_flow"] / rows["prior_total_assets"]
    )
    rows["leverage"] = rows["current_lt_debt"] / (
        (rows["current_total_assets"] + rows["prior_total_assets"]) / 2.0
    )
    rows["prior_leverage"] = rows["prior_lt_debt"] / (
        (rows["prior_total_assets"] + rows["prior2_total_assets"]) / 2.0
    )
    rows["current_ratio"] = (
        rows["current_current_assets"] / rows["current_current_liabilities"]
    )
    rows["prior_current_ratio"] = (
        rows["prior_current_assets"] / rows["prior_current_liabilities"]
    )
    rows["gross_margin"] = rows["current_gross_profit"] / rows["current_revenue"]
    rows["prior_gross_margin"] = rows["prior_gross_profit"] / rows["prior_revenue"]
    rows["asset_turnover"] = rows["current_revenue"] / rows["prior_total_assets"]
    rows["prior_asset_turnover"] = (
        rows["prior_revenue"] / rows["prior2_total_assets"]
    )

    rows["positive_roa"] = rows["roa"] > 0
    rows["positive_cfo"] = rows["cfo_to_beginning_assets"] > 0
    rows["improving_roa"] = rows["roa"] > rows["prior_roa"]
    rows["low_accruals"] = (
        rows["current_operating_cash_flow"] > rows["current_net_income"]
    )
    rows["falling_leverage"] = rows["leverage"] < rows["prior_leverage"]
    rows["improving_liquidity"] = (
        rows["current_ratio"] > rows["prior_current_ratio"]
    )
    rows["no_net_share_issuance"] = rows["low_net_issuance"] >= 0
    rows["improving_gross_margin"] = (
        rows["gross_margin"] > rows["prior_gross_margin"]
    )
    rows["improving_asset_turnover"] = (
        rows["asset_turnover"] > rows["prior_asset_turnover"]
    )
    rows["piotroski_f_score"] = rows[list(_SIGNAL_COLUMNS)].astype(int).sum(axis=1)
    rows["is_high_book_to_market"] = (
        rows["book_to_market_percentile"] >= options.high_book_to_market_percentile
    )
    ratios = [
        "roa",
        "prior_roa",
        "cfo_to_beginning_assets",
        "leverage",
        "prior_leverage",
        "current_ratio",
        "prior_current_ratio",
        "gross_margin",
        "prior_gross_margin",
        "asset_turnover",
        "prior_asset_turnover",
    ]
    finite = rows[ratios].apply(lambda column: column.map(math.isfinite)).all(axis=1)
    return rows[finite].copy()


def compute_piotroski_rows(
    inputs: pd.DataFrame,
    options: PiotroskiOptions | None = None,
) -> pd.DataFrame:
    """Calculate and standardize standalone and high-value F-score rows."""

    options = options or PiotroskiOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "book_to_market",
        "book_to_market_percentile",
        "low_net_issuance",
        "prior2_total_assets",
        *(f"current_{metric}" for metric in _ANNUAL_METRICS),
        *(f"prior_{metric}" for metric in _ANNUAL_METRICS),
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"Piotroski inputs missing columns: {missing}")

    base = inputs.copy()
    base["as_of_date"] = pd.to_datetime(base["as_of_date"], errors="coerce").dt.date
    base["available_at"] = pd.to_datetime(
        base["decision_available_at"], errors="coerce"
    )
    base = base.dropna(subset=["security_id", "as_of_date", "available_at"])
    base = calculate_piotroski_components(base, options)
    if base.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    parts: list[pd.DataFrame] = []
    for factor_id in FACTOR_IDS:
        part = base.copy()
        minimum_names = options.minimum_names_per_date
        if factor_id == HIGH_BOOK_TO_MARKET_FACTOR_ID:
            part = part[part["is_high_book_to_market"]].copy()
            minimum_names = options.minimum_high_book_to_market_names_per_date
        counts = part.groupby("as_of_date")["security_id"].transform("nunique")
        part = part[counts >= minimum_names].copy()
        if part.empty:
            continue
        metadata = FACTOR_METADATA[factor_id]
        part["factor_id"] = factor_id
        part["factor_name"] = metadata["factor_name"]
        part["family"] = metadata["family"]
        part["raw_value"] = part["piotroski_f_score"].astype(float)
        part = zscore(
            part,
            value_column="raw_value",
            output_column="value",
            partition_columns=("factor_id", "as_of_date"),
        )
        part["input_ids_json"] = json_dumps(
            [
                f"factor:{BOOK_TO_MARKET_FACTOR_ID}",
                f"factor:{NET_ISSUANCE_FACTOR_ID}",
                *(f"metric:{metric}" for metric in _ANNUAL_METRICS),
            ]
        )
        part["input_lineage_json"] = part.apply(
            lambda row, selected_factor_id=factor_id: piotroski_input_lineage(
                row, options, selected_factor_id
            ),
            axis=1,
        )
        part["is_latest_revision"] = True
        part["run_id"] = options.run_id
        part["source"] = options.source
        part["factor_value_id"] = [
            _factor_value_id(
                options.source,
                factor_id,
                security_id,
                as_of_date,
            )
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


def refresh_piotroski_values(
    store: DuckDBStore,
    options: PiotroskiOptions | None = None,
) -> int:
    """Materialize both complete-case Piotroski features."""

    options = options or PiotroskiOptions()
    store.initialize()
    rows = compute_piotroski_rows(load_piotroski_inputs(store, options), options)
    predicates = ["source = ?", "factor_id IN (?, ?)"]
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
            insert_frame(store, rows, "fundamental_factor_values", "piotroski_insert")
    return len(rows)
