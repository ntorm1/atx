"""Decile-preserving operating-profitability / net-issuance factor router.

Operating profitability remains the preferred signal. Net share issuance is used only
for a security/date where operating profitability is unavailable, expanding breadth
without averaging away the stronger input on their common cohort. Quarterly operating
cash profitability orders names only within the primary router's deciles, so it cannot
change the top- or bottom-decile membership selected by the proven production signal.
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
from .universe import DEFAULT_UNIVERSE_ID
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db governed cash-decile router v6"
FACTOR_ID = "composite_operating_profitability_or_net_issuance"
FACTOR_NAME = "Decile-preserving profitability router"
FACTOR_FAMILY = "fundamental_composite"
PRIMARY_FACTOR_ID = "profitability_operating_profitability"
FALLBACK_FACTOR_ID = "financing_low_net_share_issuance"
SECONDARY_FACTOR_ID = (
    "profitability_quarterly_cash_operating_profitability_lagged_assets"
)
PRIMARY_BUCKETS = 10
SECONDARY_SCALE = 0.999

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
class ConditionalRouterOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    minimum_names_per_date: int = 20
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_conditional_router_inputs(
    store: DuckDBStore,
    options: ConditionalRouterOptions | None = None,
) -> pd.DataFrame:
    """Choose the primary route and attach a same-date quarterly secondary key."""

    options = options or ConditionalRouterOptions()
    if options.universe_id != DEFAULT_UNIVERSE_ID:
        raise ValueError(
            "conditional router currently requires the v_factor_panel universe "
            f"{DEFAULT_UNIVERSE_ID!r}"
        )
    date_predicates: list[str] = []
    date_params: list[object] = []
    if options.start_date is not None:
        date_predicates.append("as_of_date >= ?")
        date_params.append(options.start_date)
    if options.end_date is not None:
        date_predicates.append("as_of_date <= ?")
        date_params.append(options.end_date)
    date_sql = (
        " AND " + " AND ".join(date_predicates) if date_predicates else ""
    )
    return store.con.execute(
        f"""
        WITH governed AS (
            SELECT
                security_id,
                as_of_date AS decision_date,
                factor_id AS input_factor_id,
                value AS input_value,
                available_at AS input_available_at,
                source_loaded_at AS input_source_loaded_at,
                input_lineage_json AS governed_input_lineage_json
            FROM v_factor_panel
            WHERE factor_id IN (?, ?, ?)
              {date_sql}
        ),
        raw_matches AS (
            SELECT
                g.*,
                f.factor_value_id AS input_factor_value_id,
                f.factor_name AS input_factor_name,
                f.symbol,
                f.raw_value AS input_raw_value,
                f.source AS input_source,
                row_number() OVER (
                    PARTITION BY g.input_factor_id, g.security_id, g.decision_date
                    ORDER BY f.available_at DESC,
                             f.source_loaded_at DESC,
                             f.factor_value_id DESC
                ) AS raw_rank
            FROM governed g
            JOIN fundamental_factor_values f
              ON f.factor_id = g.input_factor_id
             AND f.security_id = g.security_id
             AND f.value = g.input_value
             AND f.available_at = g.input_available_at
             AND greatest(f.as_of_date, CAST(f.available_at AS DATE)) = g.decision_date
             AND f.is_latest_revision
        ),
        eligible AS (
            SELECT * EXCLUDE (raw_rank)
            FROM raw_matches
            WHERE raw_rank = 1
        ),
        route_candidates AS (
            SELECT
                *,
                CASE WHEN input_factor_id = ? THEN 'primary' ELSE 'fallback' END AS route,
                row_number() OVER (
                    PARTITION BY security_id, decision_date
                    ORDER BY CASE WHEN input_factor_id = ? THEN 0 ELSE 1 END,
                             input_available_at DESC,
                             input_factor_value_id DESC
                ) AS route_rank
            FROM eligible
            WHERE input_factor_id IN (?, ?)
        ),
        chosen AS (
            SELECT * EXCLUDE (route_rank)
            FROM route_candidates
            WHERE route_rank = 1
        ),
        secondary AS (
            SELECT
                security_id,
                decision_date,
                input_factor_value_id AS secondary_factor_value_id,
                input_raw_value AS secondary_raw_value,
                input_value AS secondary_value,
                input_available_at AS secondary_available_at,
                input_source AS secondary_source
            FROM eligible
            WHERE input_factor_id = ?
        )
        SELECT
            c.* EXCLUDE (decision_date),
            c.decision_date AS as_of_date,
            s.secondary_factor_value_id,
            s.secondary_raw_value,
            s.secondary_value,
            s.secondary_available_at,
            s.secondary_source
        FROM chosen c
        LEFT JOIN secondary s USING (security_id, decision_date)
        ORDER BY c.decision_date, c.security_id
        """,
        [
            PRIMARY_FACTOR_ID,
            FALLBACK_FACTOR_ID,
            SECONDARY_FACTOR_ID,
            *date_params,
            PRIMARY_FACTOR_ID,
            PRIMARY_FACTOR_ID,
            PRIMARY_FACTOR_ID,
            FALLBACK_FACTOR_ID,
            SECONDARY_FACTOR_ID,
        ],
    ).df()


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "method": "primary_decile_then_quarterly_cash_profitability_order",
            "primary_factor_id": PRIMARY_FACTOR_ID,
            "fallback_factor_id": FALLBACK_FACTOR_ID,
            "secondary_factor_id": SECONDARY_FACTOR_ID,
            "selected_route": row["route"],
            "primary_bucket": int(row["primary_bucket"]),
            "primary_bucket_count": PRIMARY_BUCKETS,
            "universe_id": DEFAULT_UNIVERSE_ID,
            "universe_contract": "v_factor_panel point-in-time membership filter",
            "secondary_used": bool(row["secondary_used"]),
            "selected_factor": {
                "factor_id": row["input_factor_id"],
                "factor_value_id": row["input_factor_value_id"],
                "raw_value": row["input_raw_value"],
                "value": row["input_value"],
                "available_at": row["input_available_at"],
                "source": row["input_source"],
            },
            "secondary_factor": {
                "factor_value_id": row.get("secondary_factor_value_id"),
                "raw_value": row.get("secondary_raw_value"),
                "value": row.get("secondary_value"),
                "available_at": row.get("secondary_available_at"),
                "source": row.get("secondary_source"),
            },
            "ordering": {
                "primary_within_bucket_rank": row["primary_within_bucket_rank"],
                "secondary_within_bucket_rank": row[
                    "secondary_within_bucket_rank"
                ],
                "lexicographic_raw_value": row["raw_value"],
                "secondary_scale": SECONDARY_SCALE,
                "top_bottom_bucket_membership_preserved": True,
            },
        }
    )


def _primary_bucket(values: pd.Series) -> pd.Series:
    bucket_count = min(PRIMARY_BUCKETS, len(values))
    ranks = values.rank(method="first")
    return pd.qcut(ranks, q=bucket_count, labels=False).astype(float)


def compute_conditional_router_rows(
    inputs: pd.DataFrame,
    options: ConditionalRouterOptions | None = None,
) -> pd.DataFrame:
    """Order by primary decile, then by quarterly profitability within each decile."""

    options = options or ConditionalRouterOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "input_factor_value_id",
        "input_factor_id",
        "security_id",
        "symbol",
        "as_of_date",
        "input_value",
        "input_available_at",
        "route",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"conditional router inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["input_available_at"], errors="coerce")
    rows["input_value"] = pd.to_numeric(rows["input_value"], errors="coerce")
    rows["secondary_value"] = pd.to_numeric(
        rows["secondary_value"], errors="coerce"
    )
    rows["secondary_available_at"] = pd.to_datetime(
        rows["secondary_available_at"], errors="coerce"
    )
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", "input_value"])
    rows = rows[rows["input_value"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["primary_bucket"] = rows.groupby("as_of_date")["input_value"].transform(
        _primary_bucket
    )
    bucket_keys = ["as_of_date", "primary_bucket"]
    rows["primary_within_bucket_rank"] = rows.groupby(bucket_keys)[
        "input_value"
    ].rank(method="average", pct=True)
    rows["secondary_within_bucket_rank"] = rows.groupby(bucket_keys)[
        "secondary_value"
    ].rank(method="average", pct=True)
    rows["secondary_used"] = rows["secondary_within_bucket_rank"].notna()
    rows["secondary_within_bucket_rank"] = rows[
        "secondary_within_bucket_rank"
    ].fillna(rows["primary_within_bucket_rank"])
    rows["available_at"] = rows["available_at"].where(
        ~rows["secondary_used"],
        rows[["available_at", "secondary_available_at"]].max(axis=1),
    )
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = (
        rows["primary_bucket"]
        + SECONDARY_SCALE * rows["secondary_within_bucket_rank"]
    )
    rows = zscore(
        rows,
        value_column="raw_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        [
            f"factor:{PRIMARY_FACTOR_ID}",
            f"factor:{FALLBACK_FACTOR_ID}",
            f"factor:{SECONDARY_FACTOR_ID}",
        ]
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


def _delete_scope(store: DuckDBStore, options: ConditionalRouterOptions) -> None:
    predicates = ["factor_id = ?"]
    params: list[object] = [FACTOR_ID]
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


def refresh_conditional_router_values(
    store: DuckDBStore,
    options: ConditionalRouterOptions | None = None,
) -> int:
    """Materialize the monthly point-in-time conditional factor."""

    options = options or ConditionalRouterOptions()
    store.initialize()
    inputs = load_conditional_router_inputs(store, options)
    rows = compute_conditional_router_rows(inputs, options)
    with store.transaction():
        _delete_scope(store, options)
        if not rows.empty:
            insert_frame(
                store,
                rows,
                "fundamental_factor_values",
                "conditional_router_values_insert",
            )
    return len(rows)
