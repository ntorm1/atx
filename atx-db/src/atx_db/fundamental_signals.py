"""Production point-in-time fundamental equity signals.

The older :mod:`atx_db.factors.fundamental_families` module contains pure
factor transforms, but it intentionally does not assemble a live warehouse
panel.  This module closes that operational gap for the first research-backed
signal family: Novy-Marx gross profitability, book-to-market, and their
quality/value composite.

Rows are formed at the last available close of each calendar month.  Every
fundamental and share-count input must have been published by that close.  The
annual gross-profit leg is restricted to 330--380 day facts and is paired with
assets/equity from the same filing accession and fiscal period.  This avoids
mixing quarterly gross profit with annual balance-sheet values and avoids
restatement look-ahead.
"""

from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from typing import Any, cast

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import rank, winsorize, zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT fundamental signals v1"
FACTOR_IDS = (
    "profitability_gross_profitability",
    "profitability_operating_profitability",
    "value_book_to_market",
    "quality_value_gross_profitability",
)

FACTOR_METADATA: dict[str, dict[str, object]] = {
    "profitability_gross_profitability": {
        "factor_name": "PIT annual gross profitability",
        "family": "fundamental_profitability",
        "inputs": ("gross_profit", "total_assets"),
    },
    "profitability_operating_profitability": {
        "factor_name": "PIT Fama-French operating profitability",
        "family": "fundamental_profitability",
        "inputs": (
            "revenue",
            "cogs",
            "sga",
            "interest_expense",
            "stockholders_equity",
        ),
    },
    "value_book_to_market": {
        "factor_name": "PIT book-to-market value",
        "family": "fundamental_value",
        "inputs": ("stockholders_equity", "market_cap"),
    },
    "quality_value_gross_profitability": {
        "factor_name": "Gross-profitability quality/value composite",
        "family": "fundamental_quality_value",
        "inputs": (
            "factor:profitability_gross_profitability",
            "factor:value_book_to_market",
        ),
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


@dataclass(frozen=True)
class FundamentalSignalOptions:
    """Build scope and investability gates for monthly signal observations."""

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


def _date_filters(options: FundamentalSignalOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_fundamental_signal_inputs(
    store: DuckDBStore,
    options: FundamentalSignalOptions | None = None,
) -> pd.DataFrame:
    """Assemble monthly, filing-time-safe signal inputs from the warehouse.

    The query deliberately keeps all historical statement revisions.  At each
    rebalance close it selects the newest fiscal period visible at that time,
    then the newest visible filing for that period.  Filtering source rows to
    ``is_latest_revision`` would leak future restatements into old decisions.
    """

    options = options or FundamentalSignalOptions()
    date_sql, date_params = _date_filters(options)
    sql = f"""
        WITH annual_facts AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                period_end,
                arg_max(
                    value,
                    (available_at, revision_sequence, statement_point_id)
                ) FILTER (
                    WHERE canonical_metric = 'gross_profit'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS gross_profit,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'gross_profit'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS gross_profit_available_at,
                arg_max(
                    statement_point_id,
                    (available_at, revision_sequence, statement_point_id)
                ) FILTER (
                    WHERE canonical_metric = 'gross_profit'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS gross_profit_id,
                arg_max(
                    value,
                    (available_at, revision_sequence, statement_point_id)
                ) FILTER (WHERE canonical_metric = 'total_assets') AS total_assets,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'total_assets'
                ) AS total_assets_available_at,
                arg_max(
                    statement_point_id,
                    (available_at, revision_sequence, statement_point_id)
                ) FILTER (WHERE canonical_metric = 'total_assets') AS total_assets_id,
                arg_max(
                    value,
                    (available_at, revision_sequence, statement_point_id)
                ) FILTER (WHERE canonical_metric = 'stockholders_equity') AS stockholders_equity,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'stockholders_equity'
                ) AS stockholders_equity_available_at,
                arg_max(
                    statement_point_id,
                    (available_at, revision_sequence, statement_point_id)
                ) FILTER (WHERE canonical_metric = 'stockholders_equity') AS stockholders_equity_id,
                arg_max(value, (available_at, revision_sequence, statement_point_id)) FILTER (
                    WHERE canonical_metric = 'revenue'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS revenue,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'revenue'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS revenue_available_at,
                arg_max(statement_point_id, (available_at, revision_sequence, statement_point_id)) FILTER (
                    WHERE canonical_metric = 'revenue'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS revenue_id,
                arg_max(value, (available_at, revision_sequence, statement_point_id)) FILTER (
                    WHERE canonical_metric = 'cogs'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS cogs,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'cogs'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS cogs_available_at,
                arg_max(statement_point_id, (available_at, revision_sequence, statement_point_id)) FILTER (
                    WHERE canonical_metric = 'cogs'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS cogs_id,
                arg_max(value, (available_at, revision_sequence, statement_point_id)) FILTER (
                    WHERE canonical_metric = 'sga'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS sga,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'sga'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS sga_available_at,
                arg_max(statement_point_id, (available_at, revision_sequence, statement_point_id)) FILTER (
                    WHERE canonical_metric = 'sga'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS sga_id,
                arg_max(value, (available_at, revision_sequence, statement_point_id)) FILTER (
                    WHERE canonical_metric = 'interest_expense'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS interest_expense,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'interest_expense'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS interest_expense_available_at,
                arg_max(statement_point_id, (available_at, revision_sequence, statement_point_id)) FILTER (
                    WHERE canonical_metric = 'interest_expense'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 379
                ) AS interest_expense_id
            FROM fundamental_statement_points
            WHERE canonical_metric IN (
                'gross_profit', 'total_assets', 'stockholders_equity',
                'revenue', 'cogs', 'sga', 'interest_expense'
            )
              AND period_end IS NOT NULL
              AND accession_number IS NOT NULL
              AND form IN ('10-K', '10-K/A', '20-F', '20-F/A', '40-F', '40-F/A')
            GROUP BY security_id, accession_number, period_end
        ),
        annual AS (
            SELECT
                *,
                greatest(
                    gross_profit_available_at,
                    total_assets_available_at,
                    stockholders_equity_available_at,
                    revenue_available_at,
                    cogs_available_at,
                    sga_available_at,
                    interest_expense_available_at
                ) AS fundamental_available_at
            FROM annual_facts
            WHERE stockholders_equity > 0
              AND (
                  (gross_profit IS NOT NULL AND total_assets > 0)
                  OR (
                      revenue IS NOT NULL
                      AND (cogs IS NOT NULL OR sga IS NOT NULL OR interest_expense IS NOT NULL)
                  )
              )
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                arg_max("close", available_at) AS close,
                arg_max(volume, available_at) AS volume,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (SELECT DISTINCT security_id FROM annual)
              AND "close" > 0
              AND trade_date IS NOT NULL
              AND available_at IS NOT NULL
            GROUP BY security_id, trade_date
        ),
        price_features AS (
            SELECT
                *,
                avg("close" * volume) OVER (
                    PARTITION BY security_id
                    ORDER BY trade_date
                    ROWS BETWEEN 20 PRECEDING AND CURRENT ROW
                ) AS adv21_usd,
                row_number() OVER (
                    PARTITION BY security_id, year(trade_date), month(trade_date)
                    ORDER BY trade_date DESC
                ) AS month_rank
            FROM price_dedup
        ),
        rebalances AS (
            SELECT *
            FROM price_features
            WHERE month_rank = 1
              {date_sql}
        ),
        share_dedup AS (
            SELECT
                security_id,
                available_at,
                arg_max(
                    share_count,
                    (effective_date, revision_sequence, share_history_id)
                ) AS share_count,
                arg_max(
                    share_history_id,
                    (effective_date, revision_sequence, share_history_id)
                ) AS share_history_id,
                max(effective_date) AS effective_date
            FROM shares_outstanding_history
            WHERE share_count_type = 'shares_outstanding'
              AND share_count > 0
            GROUP BY security_id, available_at
        ),
        matched AS (
            SELECT
                p.*,
                a.period_end,
                a.accession_number,
                a.gross_profit,
                a.gross_profit_available_at,
                a.gross_profit_id,
                a.total_assets,
                a.total_assets_available_at,
                a.total_assets_id,
                a.stockholders_equity,
                a.stockholders_equity_available_at,
                a.stockholders_equity_id,
                a.revenue,
                a.revenue_available_at,
                a.revenue_id,
                a.cogs,
                a.cogs_available_at,
                a.cogs_id,
                a.sga,
                a.sga_available_at,
                a.sga_id,
                a.interest_expense,
                a.interest_expense_available_at,
                a.interest_expense_id,
                a.fundamental_available_at,
                s.share_count,
                s.share_history_id,
                s.available_at AS share_available_at,
                s.effective_date,
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY
                        a.period_end DESC,
                        a.fundamental_available_at DESC,
                        a.accession_number DESC
                ) AS annual_rank
            FROM rebalances p
            JOIN annual a
              ON a.security_id = p.security_id
             AND a.period_end <= p.trade_date
             AND a.fundamental_available_at <= p.price_available_at
            ASOF LEFT JOIN share_dedup s
              ON p.security_id = s.security_id
             AND p.price_available_at >= s.available_at
        ),
        base AS (
            SELECT
                *,
                gross_profit / total_assets AS gross_profitability,
                CASE
                    WHEN revenue IS NOT NULL
                     AND (cogs IS NOT NULL OR sga IS NOT NULL OR interest_expense IS NOT NULL)
                     AND greatest(
                         revenue_available_at,
                         cogs_available_at,
                         sga_available_at,
                         interest_expense_available_at,
                         stockholders_equity_available_at
                     ) <= price_available_at
                    THEN (
                        revenue
                        - coalesce(cogs, 0)
                        - coalesce(sga, 0)
                        - coalesce(interest_expense, 0)
                    ) / stockholders_equity
                END AS operating_profitability,
                stockholders_equity / nullif("close" * share_count, 0) AS book_to_market,
                "close" * share_count AS market_cap_usd
            FROM matched
            WHERE annual_rank = 1
              AND effective_date <= trade_date
        )
        SELECT *
        FROM base
        WHERE market_cap_usd >= ?
          AND adv21_usd >= ?
          AND trade_date - period_end <= ?
          AND book_to_market BETWEEN 0 AND 5
          AND (
              gross_profitability BETWEEN -5 AND 5
              OR operating_profitability BETWEEN -20 AND 20
          )
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
    common = {
        "decision": {
            "trade_date": row["trade_date"],
            "price_available_at": row["price_available_at"],
            "close": row["close"],
            "adv21_usd": row["adv21_usd"],
        },
        "gross_profit": {
            "table": "fundamental_statement_points",
            "id": row["gross_profit_id"],
            "value": row["gross_profit"],
            "available_at": row["gross_profit_available_at"],
            "period_end": row["period_end"],
            "accession_number": row["accession_number"],
        },
        "total_assets": {
            "table": "fundamental_statement_points",
            "id": row["total_assets_id"],
            "value": row["total_assets"],
            "available_at": row["total_assets_available_at"],
        },
        "stockholders_equity": {
            "table": "fundamental_statement_points",
            "id": row["stockholders_equity_id"],
            "value": row["stockholders_equity"],
            "available_at": row["stockholders_equity_available_at"],
        },
        "operating_profitability": {
            "formula": "(revenue - coalesce(cogs, 0) - coalesce(sga, 0) - coalesce(interest_expense, 0)) / stockholders_equity",
            "minority_interest_in_denominator": False,
            "revenue": {
                "id": row.get("revenue_id"),
                "value": row.get("revenue"),
                "available_at": row.get("revenue_available_at"),
            },
            "cogs": {
                "id": row.get("cogs_id"),
                "value": row.get("cogs"),
                "available_at": row.get("cogs_available_at"),
            },
            "sga": {
                "id": row.get("sga_id"),
                "value": row.get("sga"),
                "available_at": row.get("sga_available_at"),
            },
            "interest_expense": {
                "id": row.get("interest_expense_id"),
                "value": row.get("interest_expense"),
                "available_at": row.get("interest_expense_available_at"),
            },
        },
        "shares": {
            "table": "shares_outstanding_history",
            "id": row["share_history_id"],
            "value": row["share_count"],
            "available_at": row["share_available_at"],
        },
        "market_cap_usd": row["market_cap_usd"],
    }
    if factor_id == "profitability_gross_profitability":
        common["used_inputs"] = ["gross_profit", "total_assets"]
    elif factor_id == "profitability_operating_profitability":
        common["used_inputs"] = ["operating_profitability", "stockholders_equity"]
    elif factor_id == "value_book_to_market":
        common["used_inputs"] = ["stockholders_equity", "shares", "decision.close"]
    else:
        common["used_inputs"] = [
            "profitability_gross_profitability",
            "value_book_to_market",
        ]
    return json_dumps(common)


def compute_fundamental_signal_rows(
    inputs: pd.DataFrame,
    options: FundamentalSignalOptions | None = None,
) -> pd.DataFrame:
    """Convert monthly inputs into winsorized cross-sectional factor rows."""

    options = options or FundamentalSignalOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "price_available_at",
        "gross_profitability",
        "book_to_market",
    }
    missing = sorted(required - set(inputs.columns))
    if missing:
        raise ValueError(f"fundamental signal inputs missing columns: {missing}")

    base = inputs.copy()
    base["trade_date"] = pd.to_datetime(base["trade_date"], errors="coerce").dt.date
    base["price_available_at"] = pd.to_datetime(base["price_available_at"], errors="coerce")
    for column in ("gross_profitability", "book_to_market"):
        base[column] = pd.to_numeric(base[column], errors="coerce")
    base = base.dropna(subset=["security_id", "trade_date", "price_available_at"])
    if base.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    ranked_gp = rank(
        base.rename(columns={"gross_profitability": "raw"}),
        value_column="raw",
        output_column="gp_rank",
        partition_columns=("trade_date",),
    )["gp_rank"]
    ranked_value = rank(
        base.rename(columns={"book_to_market": "raw"}),
        value_column="raw",
        output_column="book_rank",
        partition_columns=("trade_date",),
    )["book_rank"]
    base["quality_value"] = (ranked_gp.astype(float) + ranked_value.astype(float)) / 2.0

    raw_columns = {
        "profitability_gross_profitability": "gross_profitability",
        "profitability_operating_profitability": "operating_profitability",
        "value_book_to_market": "book_to_market",
        "quality_value_gross_profitability": "quality_value",
    }
    parts: list[pd.DataFrame] = []
    for factor_id, raw_column in raw_columns.items():
        if raw_column not in base.columns:
            continue
        metadata = FACTOR_METADATA[factor_id]
        part = base.dropna(subset=[raw_column]).copy()
        date_counts = part.groupby("trade_date")["security_id"].transform("nunique")
        part = part[date_counts >= options.minimum_names_per_date].copy()
        if part.empty:
            continue
        part["factor_id"] = factor_id
        part["factor_name"] = cast(str, metadata["factor_name"])
        part["family"] = cast(str, metadata["family"])
        part["raw_value"] = part[raw_column].astype(float)
        part["as_of_date"] = part["trade_date"]
        part = winsorize(
            part,
            value_column="raw_value",
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
        part["input_ids_json"] = json_dumps(list(cast(tuple[str, ...], metadata["inputs"])))
        part["input_lineage_json"] = part.apply(
            lambda row, current_factor_id=factor_id: _lineage(row, current_factor_id),
            axis=1,
        )
        part["is_latest_revision"] = True
        part["run_id"] = options.run_id
        part["source"] = options.source
        part["factor_value_id"] = [
            _factor_value_id(options.source, factor_id, security_id, as_of_date)
            for security_id, as_of_date in zip(part["security_id"], part["as_of_date"], strict=True)
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


def _delete_scope(store: DuckDBStore, options: FundamentalSignalOptions) -> None:
    predicates = ["source = ?", f"factor_id IN ({', '.join('?' for _ in FACTOR_IDS)})"]
    params: list[object] = [options.source, *FACTOR_IDS]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    store.con.execute(
        f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}",
        params,
    )


def refresh_fundamental_signal_values(
    store: DuckDBStore,
    options: FundamentalSignalOptions | None = None,
) -> int:
    """Materialize the production monthly profitability/value factor layer."""

    options = options or FundamentalSignalOptions()
    store.initialize()
    inputs = load_fundamental_signal_inputs(store, options)
    rows = compute_fundamental_signal_rows(inputs, options)
    with store.transaction():
        _delete_scope(store, options)
        if not rows.empty:
            insert_frame(
                store,
                rows,
                "fundamental_factor_values",
                "fundamental_signal_values_insert",
            )
    return len(rows)
