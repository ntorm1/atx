from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import file_sha256, insert_frame, json_dumps, quality_check, record_source_file, snake_case, symbol_key


SOURCE_NAME = "ATX public delisting proxy builder"
DEFAULT_SOURCE = "atx_delisting_proxy_v1"
DEFAULT_CODE_SOURCE = "atx_delist_code_dim_v1"


@dataclass(frozen=True)
class DelistingEventOptions:
    source: str = DEFAULT_SOURCE
    listing_status_source: str | None = None
    include_snapshot_absence: bool = False
    apply_shumway_warther_imputation: bool = False
    run_id: str | None = None


@dataclass(frozen=True)
class DelistingReturnObservationOptions:
    source_file: Path | None = None
    source: str = "injected_delisting_return_observations_v1"
    provider: str = "INJECTED"
    vendor_security_id_type: str = "PERMNO"
    replace_source_file: bool = True
    run_id: str | None = None


DELIST_CODE_ROWS = (
    (
        "NASDAQ_DELETE",
        "ATX_PUBLIC_PROXY",
        None,
        None,
        "UNKNOWN_PUBLIC_DELETE",
        "exchange_delete",
        (
            "Nasdaq Trader add/delete file delete action. This is public listing-status evidence, "
            "not an official CRSP DLSTCD reason code."
        ),
        "DELISTED_OR_TRANSFERRED_UNKNOWN",
        True,
        -0.30,
        "optional_shumway_warther_unresolved_delete_minus_30pct",
        DEFAULT_CODE_SOURCE,
    ),
    (
        "SNAPSHOT_ABSENCE",
        "ATX_PUBLIC_PROXY",
        None,
        None,
        "UNKNOWN_SNAPSHOT_GAP",
        "snapshot_absence",
        (
            "Symbol disappeared from consecutive public symbol-directory snapshots. This is lower "
            "confidence absence evidence and should not be treated as an official delisting reason."
        ),
        "ABSENT_FROM_PUBLIC_DIRECTORY",
        False,
        None,
        "none",
        DEFAULT_CODE_SOURCE,
    ),
)

OBSERVATION_COLUMNS = [
    "delisting_return_observation_id",
    "source",
    "provider",
    "source_file",
    "source_file_sha256",
    "security_id",
    "symbol",
    "vendor_security_id",
    "vendor_security_id_type",
    "delist_date",
    "as_of_date",
    "available_at",
    "delist_code",
    "vendor_delist_code",
    "crsp_dlstcd",
    "delist_amount",
    "delist_price",
    "delisting_return",
    "delisting_return_ex_div",
    "delist_pay_date",
    "next_pricing_date",
    "successor_security_id",
    "successor_vendor_security_id",
    "return_basis",
    "currency",
    "raw_payload_json",
    "run_id",
]

COLUMN_ALIASES = {
    "permno": "vendor_security_id",
    "gvkey": "vendor_security_id",
    "fsym_id": "vendor_security_id",
    "fsymid": "vendor_security_id",
    "ticker": "symbol",
    "tic": "symbol",
    "dlstdt": "delist_date",
    "dldte": "delist_date",
    "delisting_date": "delist_date",
    "dlstcd": "crsp_dlstcd",
    "dlrsn": "vendor_delist_code",
    "dlamt": "delist_amount",
    "dlprc": "delist_price",
    "dlret": "delisting_return",
    "dlretx": "delisting_return_ex_div",
    "dlpdt": "delist_pay_date",
    "nextdt": "next_pricing_date",
    "nwperm": "successor_vendor_security_id",
    "nwcomp": "successor_security_id",
    "asof_date": "as_of_date",
    "knowledge_from": "available_at",
    "source_loaded_at": "available_at",
}


def seed_delist_code_dim(store: DuckDBStore, *, source: str = DEFAULT_CODE_SOURCE) -> int:
    store.initialize()
    rows = [row for row in DELIST_CODE_ROWS if row[-1] == source]
    if not rows:
        return 0
    with store.transaction():
        store.con.execute("DELETE FROM delist_code_dim WHERE source = ?", [source])
        store.con.executemany(
            """
            INSERT INTO delist_code_dim (
                delist_code,
                code_system,
                vendor_code,
                crsp_dlstcd,
                crsp_dlstcd_family,
                reason_category,
                description,
                terminal_trading_status,
                imputation_allowed,
                default_imputed_return,
                imputation_policy,
                source
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            rows,
        )
    return len(rows)


def _empty_observation_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=OBSERVATION_COLUMNS)


def _normalize_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = COLUMN_ALIASES.get(normalized, COLUMN_ALIASES.get(compact, normalized))
    return frame.rename(columns=renamed)


def _date_series(frame: pd.DataFrame, column: str, fallback: dt.date | None = None) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([fallback] * len(frame), index=frame.index, dtype="object")
    parsed = pd.to_datetime(frame[column].replace("", pd.NA), errors="coerce")
    if fallback is not None:
        parsed = parsed.fillna(pd.Timestamp(fallback))
    return parsed.dt.date


def _timestamp_series(frame: pd.DataFrame, column: str, fallback: dt.datetime) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([fallback] * len(frame), index=frame.index, dtype="datetime64[ns]")
    parsed = pd.to_datetime(frame[column].replace("", pd.NA), errors="coerce")
    return parsed.fillna(pd.Timestamp(fallback))


def _string_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="string")
    return frame[column].replace("", pd.NA).astype("string")


def _numeric_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="Float64")
    return pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce")


def _int_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="Int64")
    return pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce").astype("Int64")


def _raw_payloads(frame: pd.DataFrame) -> pd.Series:
    return frame.apply(lambda row: json_dumps(row.dropna().to_dict()), axis=1)


def _stable_observation_id(row: pd.Series) -> str:
    parts = [
        row.get("source"),
        row.get("provider"),
        row.get("security_id"),
        row.get("symbol"),
        row.get("vendor_security_id_type"),
        row.get("vendor_security_id"),
        row.get("delist_date"),
        row.get("delisting_return"),
        row.get("source_file_sha256"),
    ]
    payload = "|".join("" if pd.isna(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def normalize_delisting_return_observations(
    frame: pd.DataFrame,
    *,
    options: DelistingReturnObservationOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_observation_frame()

    raw = _normalize_columns(frame.copy())
    if "delist_date" not in raw.columns:
        raise ValueError("Delisting return observations require delist_date/DLSTDT")
    if "delisting_return" not in raw.columns:
        raise ValueError("Delisting return observations require delisting_return/DLRET")

    now = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
    delist_date = _date_series(raw, "delist_date")
    as_of_date = _date_series(raw, "as_of_date")
    as_of_date = as_of_date.where(pd.notna(as_of_date), delist_date)
    available_at = _timestamp_series(raw, "available_at", now)

    normalized = pd.DataFrame(index=raw.index)
    normalized["source"] = _string_series(raw, "source").fillna(options.source)
    normalized["provider"] = _string_series(raw, "provider").fillna(options.provider).str.upper()
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["security_id"] = _string_series(raw, "security_id")
    normalized["symbol"] = _string_series(raw, "symbol").map(lambda value: symbol_key(None if pd.isna(value) else str(value)))
    normalized["symbol"] = normalized["symbol"].replace("", pd.NA)
    normalized["vendor_security_id"] = _string_series(raw, "vendor_security_id")
    normalized["vendor_security_id_type"] = _string_series(raw, "vendor_security_id_type").fillna(
        options.vendor_security_id_type
    ).str.upper()
    normalized["delist_date"] = delist_date
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["crsp_dlstcd"] = _int_series(raw, "crsp_dlstcd")
    normalized["delist_code"] = _string_series(raw, "delist_code")
    normalized["vendor_delist_code"] = _string_series(raw, "vendor_delist_code").fillna(
        normalized["crsp_dlstcd"].astype("string")
    )
    normalized["delist_amount"] = _numeric_series(raw, "delist_amount")
    normalized["delist_price"] = _numeric_series(raw, "delist_price")
    normalized["delisting_return"] = _numeric_series(raw, "delisting_return")
    normalized["delisting_return_ex_div"] = _numeric_series(raw, "delisting_return_ex_div")
    normalized["delist_pay_date"] = _date_series(raw, "delist_pay_date")
    normalized["next_pricing_date"] = _date_series(raw, "next_pricing_date")
    normalized["successor_security_id"] = _string_series(raw, "successor_security_id")
    normalized["successor_vendor_security_id"] = _string_series(raw, "successor_vendor_security_id")
    normalized["return_basis"] = _string_series(raw, "return_basis").fillna("CRSP_DLRET")
    normalized["currency"] = _string_series(raw, "currency")
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = options.run_id
    normalized = normalized[
        normalized["delist_date"].notna()
        & normalized["as_of_date"].notna()
        & normalized["available_at"].notna()
        & normalized["delisting_return"].notna()
    ].copy()
    if normalized.empty:
        return _empty_observation_frame()
    normalized["delisting_return_observation_id"] = normalized.apply(_stable_observation_id, axis=1)
    return normalized[OBSERVATION_COLUMNS]


def _resolve_observation_security_ids(store: DuckDBStore, observations: pd.DataFrame) -> pd.DataFrame:
    if observations.empty:
        return observations
    out = observations.copy().reset_index(drop=True)
    unresolved = (
        out["security_id"].isna()
        & out["vendor_security_id"].notna()
        & out["vendor_security_id_type"].notna()
        & out["delist_date"].notna()
        & out["available_at"].notna()
    )
    if not bool(unresolved.any()):
        return observations

    relation_name = "delisting_return_observation_resolution_input"
    lookup = out.loc[unresolved, [
        "vendor_security_id",
        "vendor_security_id_type",
        "delist_date",
        "as_of_date",
        "available_at",
    ]].copy()
    lookup["__row_number"] = lookup.index
    store.con.register(relation_name, lookup)
    try:
        resolved = store.con.execute(
            f"""
            WITH ranked AS (
                SELECT
                    o.__row_number,
                    h.security_id,
                    row_number() OVER (
                        PARTITION BY o.__row_number
                        ORDER BY
                            h.available_at DESC NULLS LAST,
                            h.valid_from DESC,
                            h.source_loaded_at DESC,
                            h.security_id DESC
                    ) AS rn
                FROM {relation_name} o
                JOIN security_identifier_history h
                  ON upper(h.id_type) = upper(o.vendor_security_id_type)
                 AND upper(h.id_value) = upper(o.vendor_security_id)
                 AND h.valid_from <= o.delist_date
                 AND coalesce(h.valid_to, DATE '9999-12-31') > o.delist_date
                 AND h.as_of_date <= o.as_of_date
                 AND (h.available_at IS NULL OR h.available_at <= o.available_at)
            )
            SELECT __row_number, security_id
            FROM ranked
            WHERE rn = 1
            """
        ).df()
    finally:
        store.con.unregister(relation_name)

    if resolved.empty:
        return observations
    mapping = dict(zip(resolved["__row_number"], resolved["security_id"], strict=True))
    target_index = out.index.intersection(mapping.keys())
    out.loc[target_index, "security_id"] = [mapping[index] for index in target_index]
    return out[OBSERVATION_COLUMNS]


def load_delisting_return_observations(
    store: DuckDBStore,
    options: DelistingReturnObservationOptions,
) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    normalized = normalize_delisting_return_observations(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    normalized = _resolve_observation_security_ids(store, normalized)
    record_source_file(
        store,
        dataset_id="delisting_return_observations",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"provider": options.provider, "rows": int(len(frame))},
    )
    if normalized.empty:
        return 0
    with store.transaction():
        if options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM delisting_return_observations
                WHERE source = ?
                  AND provider = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, options.provider.upper(), source_hash],
            )
        insert_frame(
            store,
            normalized,
            "delisting_return_observations",
            "delisting_return_observations_insert",
        )
    return int(len(normalized))


def refresh_delisting_events(
    store: DuckDBStore,
    options: DelistingEventOptions | None = None,
) -> int:
    """Materialize conservative public delisting evidence from listing-status intervals."""

    options = options or DelistingEventOptions()
    store.initialize()
    seed_delist_code_dim(store)

    with store.transaction():
        store.con.execute(
            """
            DELETE FROM delisting_events
            WHERE source = ?
              AND (? IS NULL OR listing_status_source = ?)
            """,
            [options.source, options.listing_status_source, options.listing_status_source],
        )
        store.con.execute(
            """
            INSERT INTO delisting_events (
                delisting_event_id,
                source,
                listing_status_source,
                source_listing_status_id,
                security_id,
                symbol,
                listing_venue_code,
                listing_venue_name,
                listing_exchange_code,
                delist_date,
                as_of_date,
                available_at,
                delist_code,
                delist_reason,
                delisting_return,
                delisting_return_type,
                is_return_imputed,
                return_policy,
                return_confidence,
                return_observation_id,
                return_observation_source,
                return_observation_provider,
                evidence_source,
                evidence_source_table,
                source_event_id,
                source_url,
                method,
                evidence_confidence,
                inferred_from_absence,
                details_json,
                run_id
            )
            WITH params AS (
                SELECT
                    ? AS source,
                    ? AS listing_status_source,
                    CAST(? AS BOOLEAN) AS include_snapshot_absence,
                    CAST(? AS BOOLEAN) AS apply_imputation,
                    ? AS run_id
            ),
            candidates AS (
                SELECT
                    l.*,
                    'NASDAQ_DELETE' AS delist_code,
                    l.valid_from AS delist_date,
                    coalesce(l.as_of_date, l.valid_from) AS event_as_of_date,
                    coalesce(l.available_at, l.last_evidence_at) AS event_available_at,
                    'trading_system_delete_action' AS delisting_method,
                    'high' AS evidence_confidence,
                    false AS inferred_from_absence
                FROM listing_status_intervals l
                CROSS JOIN params p
                WHERE lower(l.status) = 'inactive'
                  AND l.valid_from IS NOT NULL
                  AND (p.listing_status_source IS NULL OR l.source = p.listing_status_source)

                UNION ALL

                SELECT
                    l.*,
                    'SNAPSHOT_ABSENCE' AS delist_code,
                    l.valid_to AS delist_date,
                    coalesce(l.last_evidence_as_of_date, l.as_of_date, l.valid_to) AS event_as_of_date,
                    coalesce(l.last_evidence_at, l.available_at) AS event_available_at,
                    'snapshot_presence_gap_absence' AS delisting_method,
                    'low' AS evidence_confidence,
                    true AS inferred_from_absence
                FROM listing_status_intervals l
                CROSS JOIN params p
                WHERE p.include_snapshot_absence
                  AND lower(l.status) = 'active'
                  AND l.valid_to IS NOT NULL
                  AND (p.listing_status_source IS NULL OR l.source = p.listing_status_source)
            ),
            enriched AS (
                SELECT
                    p.source,
                    c.source AS listing_status_source,
                    c.listing_status_id,
                    c.security_id,
                    c.symbol,
                    c.listing_venue_code,
                    c.listing_venue_name,
                    c.listing_exchange_code,
                    c.delist_date,
                    c.event_as_of_date AS as_of_date,
                    coalesce(
                        c.event_available_at,
                        CAST(c.event_as_of_date AS TIMESTAMP) + INTERVAL '22 hours'
                    ) AS available_at,
                    c.delist_code,
                    d.description AS delist_reason,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN d.default_imputed_return
                        ELSE NULL
                    END AS delisting_return,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN d.imputation_policy
                        ELSE 'UNOBSERVED_PUBLIC_PROXY'
                    END AS delisting_return_type,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN true
                        ELSE false
                    END AS is_return_imputed,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN d.imputation_policy
                        ELSE 'none'
                    END AS return_policy,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN 'low'
                        ELSE 'none'
                    END AS return_confidence,
                    NULL AS return_observation_id,
                    NULL AS return_observation_source,
                    NULL AS return_observation_provider,
                    c.evidence_source,
                    c.evidence_source_table,
                    c.source_event_id,
                    c.source_url,
                    c.delisting_method AS method,
                    c.evidence_confidence,
                    c.inferred_from_absence,
                    c.details_json,
                    p.run_id
                FROM candidates c
                CROSS JOIN params p
                JOIN delist_code_dim d
                  ON d.delist_code = c.delist_code
            )
            SELECT
                sha256(
                    concat_ws(
                        '|',
                        source,
                        listing_status_source,
                        listing_status_id,
                        delist_code,
                        CAST(delist_date AS VARCHAR)
                    )
                ) AS delisting_event_id,
                source,
                listing_status_source,
                listing_status_id AS source_listing_status_id,
                security_id,
                symbol,
                listing_venue_code,
                listing_venue_name,
                listing_exchange_code,
                delist_date,
                as_of_date,
                available_at,
                delist_code,
                delist_reason,
                delisting_return,
                delisting_return_type,
                is_return_imputed,
                return_policy,
                return_confidence,
                return_observation_id,
                return_observation_source,
                return_observation_provider,
                evidence_source,
                evidence_source_table,
                source_event_id,
                source_url,
                method,
                evidence_confidence,
                inferred_from_absence,
                details_json,
                coalesce(run_id, ?)
            FROM enriched
            """,
            [
                options.source,
                options.listing_status_source,
                options.include_snapshot_absence,
                options.apply_shumway_warther_imputation,
                options.run_id,
                options.run_id,
            ],
        )

    return int(
        store.con.execute(
            """
            SELECT count(*)
            FROM delisting_events
            WHERE source = ?
              AND (? IS NULL OR listing_status_source = ?)
            """,
            [options.source, options.listing_status_source, options.listing_status_source],
        ).fetchone()[0]
    )


class DelistingEventDataset(Dataset):
    dataset_id = "delisting_events"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: DelistingEventOptions) -> DatasetLoadResult:
        rows = refresh_delisting_events(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="delisting_events",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "listing_status_source": options.listing_status_source,
                "include_snapshot_absence": options.include_snapshot_absence,
                "apply_shumway_warther_imputation": options.apply_shumway_warther_imputation,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "listing_status_source": options.listing_status_source,
                "include_snapshot_absence": options.include_snapshot_absence,
                "apply_shumway_warther_imputation": options.apply_shumway_warther_imputation,
            },
        )


# ---------------------------------------------------------------------------
# PF4-S4 S4-0: observed DLRET terminal-return catalog + DLSTCD reconciliation.
#
# delisting_return_observations (above) holds the raw vendor DLRET rows. This section
# collapses them to exactly one terminal return per delisted (security_id, delist_date) that
# later PF4-S4 tasks stitch into the forward-return series, and reconciles (reports, never
# overwrites) the vendor DLSTCD against the warehouse's own public delist_code proxy.
# terminal_return_source is 'observed' only here; S4-1 adds the deterministic corporate-action
# 'policy' path via terminal_return_policy_dim (created empty by migration 0185). 'imputed' is
# never written to delisting_terminal_returns.
# ---------------------------------------------------------------------------

DEFAULT_TERMINAL_RETURN_SOURCE = "atx_delisting_terminal_return_v1"
DEFAULT_RECONCILIATION_SOURCE = "atx_delisting_code_reconciliation_v1"

TERMINAL_RETURN_COLUMNS = [
    "terminal_return_id",
    "source",
    "security_id",
    "symbol",
    "delist_date",
    "as_of_date",
    "available_at",
    "terminal_return",
    "terminal_return_ex_div",
    "terminal_return_source",
    "terminal_return_policy",
    "crsp_dlstcd",
    "return_basis",
    "successor_security_id",
    "return_observation_id",
    "run_id",
]

RECONCILIATION_COLUMNS = [
    "reconciliation_id",
    "source",
    "security_id",
    "symbol",
    "delist_date",
    "as_of_date",
    "available_at",
    "warehouse_delist_code",
    "warehouse_reason_category",
    "vendor_crsp_dlstcd",
    "vendor_dlstcd_family",
    "reconciliation_status",
    "mismatch_reason",
    "delisting_event_id",
    "delisting_return_observation_id",
    "run_id",
]

# Coarse CRSP DLSTCD -> family mapping: 2xx merger, 3xx exchange, 4xx liquidation, 5xx dropped.
_DLSTCD_FAMILY_BY_PREFIX = {2: "merger", 3: "exchange", 4: "liquidation", 5: "dropped"}

# The warehouse's own public delist_code proxy is built from listing-status deletes, not a
# corporate-action feed: it can only ever assert "this name stopped trading", never *why*. It is
# therefore only compatible with vendor DLSTCD families that likewise carry no distinguishing
# corporate action (a plain exchange delete / dropped-for-cause). A vendor "merger" or
# "liquidation" is a real disagreement the generic proxy could not have seen on its own, and must
# be surfaced as a mismatch -- this is exactly the invisible-disagreement gap S4-0 fixes.
RECONCILIATION_COMPATIBLE_FAMILIES = {
    "exchange_delete": frozenset({"exchange", "dropped"}),
    "snapshot_absence": frozenset({"exchange", "dropped"}),
}


@dataclass(frozen=True)
class DelistingTerminalReturnOptions:
    source: str = DEFAULT_TERMINAL_RETURN_SOURCE
    run_id: str | None = None


@dataclass(frozen=True)
class DelistingCodeReconciliationOptions:
    source: str = DEFAULT_RECONCILIATION_SOURCE
    run_id: str | None = None


def _empty_terminal_return_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=TERMINAL_RETURN_COLUMNS)


def _stable_terminal_return_id(row: pd.Series) -> str:
    parts = [row.get("source"), row.get("security_id"), row.get("delist_date"), row.get("terminal_return_source")]
    payload = "|".join("" if pd.isna(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def compute_delisting_terminal_returns(
    observations: pd.DataFrame,
    events: pd.DataFrame,
    policy_dim: pd.DataFrame,
    *,
    source: str = DEFAULT_TERMINAL_RETURN_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Collapse ``delisting_return_observations`` to one terminal return per
    ``(security_id, delist_date)``.

    The latest-visible observation wins: ``ORDER BY available_at DESC, source_loaded_at
    DESC, delisting_return_observation_id DESC`` -- the same tie-break
    ``DELISTING_EVENTS_ASOF_SQL``'s ``observation_candidates`` CTE already uses (see
    ``db/asof/security.py``). Every emitted row is tagged ``terminal_return_source='observed'``;
    ``policy_dim`` is accepted for signature stability so S4-1 can add the deterministic
    corporate-action policy path without changing this function's call sites. ``imputed`` is
    never written here.

    ``available_at`` is inherited verbatim from the observation's ``available_at`` (the
    delisting-confirmation timestamp), never the delist event date -- the no-lookahead
    invariant the survivorship fix depends on.
    """

    del policy_dim  # unused in S4-0; S4-1 wires the deterministic policy path through here.

    if observations.empty:
        return _empty_terminal_return_frame()

    obs = observations.copy().reset_index(drop=True)

    # Backfill a missing observation security_id from the public delisting-events proxy using
    # the same (delist_date, symbol) fallback the observation_candidates CTE in
    # DELISTING_EVENTS_ASOF_SQL uses when an observation carries no security_id of its own.
    if (
        not events.empty
        and "security_id" in obs.columns
        and "symbol" in obs.columns
        and {"security_id", "symbol", "delist_date"}.issubset(events.columns)
    ):
        missing = obs["security_id"].isna() & obs["symbol"].notna()
        if bool(missing.any()):
            proxy = (
                events[["security_id", "symbol", "delist_date"]]
                .dropna(subset=["security_id", "symbol", "delist_date"])
                .drop_duplicates(subset=["symbol", "delist_date"])
            )
            backfilled = obs.loc[missing, ["symbol", "delist_date"]].merge(
                proxy, on=["symbol", "delist_date"], how="left"
            )
            obs.loc[missing, "security_id"] = backfilled["security_id"].to_numpy()

    obs = obs[
        obs["security_id"].notna() & obs["delist_date"].notna() & obs["delisting_return"].notna()
    ].copy()
    if obs.empty:
        return _empty_terminal_return_frame()

    if "source_loaded_at" not in obs.columns:
        obs["source_loaded_at"] = pd.NaT

    # Stable collapse: latest-visible observation wins per (security_id, delist_date).
    # kind="mergesort" is a stable sort so ties beyond the explicit tie-break columns still
    # resolve deterministically -- required for "same inputs -> byte-identical rows".
    obs = obs.sort_values(
        by=["security_id", "delist_date", "available_at", "source_loaded_at", "delisting_return_observation_id"],
        ascending=[True, True, False, False, False],
        kind="mergesort",
        na_position="last",
    )
    winners = obs.drop_duplicates(subset=["security_id", "delist_date"], keep="first").reset_index(drop=True)

    result = pd.DataFrame(
        {
            "source": source,
            "security_id": winners["security_id"],
            "symbol": winners["symbol"] if "symbol" in winners.columns else pd.NA,
            "delist_date": winners["delist_date"],
            "as_of_date": winners["as_of_date"] if "as_of_date" in winners.columns else winners["delist_date"],
            "available_at": winners["available_at"],
            "terminal_return": winners["delisting_return"],
            "terminal_return_ex_div": (
                winners["delisting_return_ex_div"] if "delisting_return_ex_div" in winners.columns else pd.NA
            ),
            "terminal_return_source": "observed",
            "terminal_return_policy": pd.NA,
            "crsp_dlstcd": winners["crsp_dlstcd"] if "crsp_dlstcd" in winners.columns else pd.NA,
            "return_basis": winners["return_basis"] if "return_basis" in winners.columns else pd.NA,
            "successor_security_id": (
                winners["successor_security_id"] if "successor_security_id" in winners.columns else pd.NA
            ),
            "return_observation_id": winners["delisting_return_observation_id"],
            "run_id": run_id,
        }
    )
    result["terminal_return_id"] = result.apply(_stable_terminal_return_id, axis=1)
    return result[TERMINAL_RETURN_COLUMNS]


def _empty_reconciliation_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=RECONCILIATION_COLUMNS)


def _dlstcd_family(code: object) -> str | None:
    try:
        if pd.isna(code):
            return None
    except (TypeError, ValueError):
        pass
    try:
        value = int(code)
    except (TypeError, ValueError):
        return None
    return _DLSTCD_FAMILY_BY_PREFIX.get(value // 100)


def _reconciliation_match_key(security_id: object, symbol: object) -> object:
    if pd.notna(security_id):
        return f"SID:{security_id}"
    if pd.notna(symbol):
        return f"SYM:{symbol}"
    return pd.NA


def _coalesce_later(left: object, right: object) -> object:
    if pd.isna(left):
        return right
    if pd.isna(right):
        return left
    return left if left >= right else right


def _reconciliation_status(
    *, has_event: bool, has_observation: bool, vendor_family: object, warehouse_reason: object
) -> tuple[str, str | None]:
    if has_event and not has_observation:
        return "warehouse_only", None
    if has_observation and not has_event:
        return "vendor_only", None
    if pd.isna(vendor_family):
        return "unmapped", "vendor_dlstcd_family_unresolved"
    compatible = RECONCILIATION_COMPATIBLE_FAMILIES.get(warehouse_reason)
    if compatible is None:
        return "unmapped", "warehouse_reason_category_unmapped"
    if vendor_family in compatible:
        return "match", None
    return "mismatch", f"vendor_family={vendor_family}_vs_warehouse_reason={warehouse_reason}"


def _stable_reconciliation_id(row: pd.Series) -> str:
    # security_id/symbol/delist_date alone are not unique: two delisting_events rows can
    # legitimately share a security-day (e.g. a listing-status delete and a snapshot-absence
    # event on the same day), and reconciliation_id is this table's PRIMARY KEY. Folding in
    # the event/observation identifiers -- rendering a missing side as '' so the id stays
    # stable for a given logical row across runs -- disambiguates without breaking determinism.
    parts = [
        row.get("source"),
        row.get("security_id"),
        row.get("symbol"),
        row.get("delist_date"),
        row.get("delisting_event_id"),
        row.get("delisting_return_observation_id"),
    ]
    payload = "|".join("" if pd.isna(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def compute_delisting_code_reconciliation(
    events: pd.DataFrame,
    terminal_returns: pd.DataFrame,
    code_dim: pd.DataFrame,
    *,
    source: str = DEFAULT_RECONCILIATION_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Reconcile (report only, never overwrite) the vendor DLSTCD against the warehouse's own
    public ``delist_code`` proxy, joined on ``(security_id, delist_date)`` with a symbol
    fallback when either side carries no ``security_id``.

    ``reconciliation_status`` is one of ``match | mismatch | vendor_only | warehouse_only |
    unmapped``. ``mismatch`` is an expected, non-failing signal that a vendor DLSTCD family
    disagrees with the warehouse's reason category; only ``unmapped`` -- a DLSTCD this function
    cannot even coarse-map, or a warehouse reason category with no known-compatible family --
    is a genuine gap. The warehouse ``delist_code`` is read-only input here; it is never
    rewritten.
    """

    if events.empty and terminal_returns.empty:
        return _empty_reconciliation_frame()

    event_columns = [
        "delisting_event_id", "security_id", "symbol", "delist_date", "as_of_date",
        "available_at", "delist_code",
    ]
    obs_columns = [
        "return_observation_id", "security_id", "symbol", "delist_date", "as_of_date",
        "available_at", "crsp_dlstcd",
    ]

    ev = events.copy().reset_index(drop=True) if not events.empty else pd.DataFrame(columns=event_columns)
    tr = (
        terminal_returns.copy().reset_index(drop=True)
        if not terminal_returns.empty
        else pd.DataFrame(columns=obs_columns)
    )

    if not ev.empty and not code_dim.empty and "delist_code" in ev.columns:
        ev = ev.merge(code_dim[["delist_code", "reason_category"]], on="delist_code", how="left")
    else:
        ev["reason_category"] = pd.NA

    ev["match_key"] = [
        _reconciliation_match_key(sid, sym)
        for sid, sym in zip(ev.get("security_id", pd.Series(dtype=object)), ev.get("symbol", pd.Series(dtype=object)))
    ]
    tr["match_key"] = [
        _reconciliation_match_key(sid, sym)
        for sid, sym in zip(tr.get("security_id", pd.Series(dtype=object)), tr.get("symbol", pd.Series(dtype=object)))
    ]

    ev = ev.rename(columns={
        "security_id": "security_id_event",
        "symbol": "symbol_event",
        "as_of_date": "as_of_date_event",
        "available_at": "available_at_event",
    })
    tr = tr.rename(columns={
        "security_id": "security_id_obs",
        "symbol": "symbol_obs",
        "as_of_date": "as_of_date_obs",
        "available_at": "available_at_obs",
    })

    merged = ev.merge(tr, on=["match_key", "delist_date"], how="outer")
    if merged.empty:
        return _empty_reconciliation_frame()

    merged["security_id"] = merged["security_id_event"].where(
        merged["security_id_event"].notna(), merged["security_id_obs"]
    )
    merged["symbol"] = merged["symbol_event"].where(merged["symbol_event"].notna(), merged["symbol_obs"])
    merged["as_of_date"] = [
        _coalesce_later(a, b) for a, b in zip(merged["as_of_date_event"], merged["as_of_date_obs"])
    ]
    merged["available_at"] = [
        _coalesce_later(a, b) for a, b in zip(merged["available_at_event"], merged["available_at_obs"])
    ]
    merged["vendor_dlstcd_family"] = [
        _dlstcd_family(code) for code in merged.get("crsp_dlstcd", pd.Series(dtype=object))
    ]

    statuses = [
        _reconciliation_status(
            has_event=pd.notna(event_id),
            has_observation=pd.notna(obs_id),
            vendor_family=family,
            warehouse_reason=reason,
        )
        for event_id, obs_id, family, reason in zip(
            merged.get("delisting_event_id", pd.Series(dtype=object)),
            merged.get("return_observation_id", pd.Series(dtype=object)),
            merged["vendor_dlstcd_family"],
            merged.get("reason_category", pd.Series(dtype=object)),
        )
    ]
    merged["reconciliation_status"] = [status for status, _ in statuses]
    merged["mismatch_reason"] = [reason for _, reason in statuses]

    result = pd.DataFrame(
        {
            "source": source,
            "security_id": merged["security_id"],
            "symbol": merged["symbol"],
            "delist_date": merged["delist_date"],
            "as_of_date": merged["as_of_date"],
            "available_at": merged["available_at"],
            "warehouse_delist_code": merged.get("delist_code"),
            "warehouse_reason_category": merged.get("reason_category"),
            "vendor_crsp_dlstcd": merged.get("crsp_dlstcd"),
            "vendor_dlstcd_family": merged["vendor_dlstcd_family"],
            "reconciliation_status": merged["reconciliation_status"],
            "mismatch_reason": merged["mismatch_reason"],
            "delisting_event_id": merged.get("delisting_event_id"),
            "delisting_return_observation_id": merged.get("return_observation_id"),
            "run_id": run_id,
        }
    )
    result = result.sort_values(
        by=["security_id", "delist_date", "symbol"], kind="mergesort", na_position="last"
    ).reset_index(drop=True)
    result["reconciliation_id"] = result.apply(_stable_reconciliation_id, axis=1)
    return result[RECONCILIATION_COLUMNS]


def refresh_delisting_terminal_returns(
    store: DuckDBStore,
    options: DelistingTerminalReturnOptions | None = None,
) -> int:
    """Materialize ``delisting_terminal_returns`` from the landed observation/event/policy
    tables via :func:`compute_delisting_terminal_returns`, replacing prior rows by source.
    """

    options = options or DelistingTerminalReturnOptions()
    store.initialize()

    observations = store.con.execute(
        """
        SELECT
            delisting_return_observation_id,
            source,
            security_id,
            symbol,
            delist_date,
            as_of_date,
            available_at,
            source_loaded_at,
            crsp_dlstcd,
            delisting_return,
            delisting_return_ex_div,
            return_basis,
            successor_security_id
        FROM delisting_return_observations
        """
    ).df()
    events = store.con.execute(
        """
        SELECT delisting_event_id, security_id, symbol, delist_date, as_of_date, available_at, delist_code
        FROM delisting_events
        """
    ).df()
    policy_dim = store.con.execute(
        """
        SELECT
            policy_code, corporate_action_type, terminal_return_basis,
            combine_successor, default_return, is_observed_required, description
        FROM terminal_return_policy_dim
        """
    ).df()

    terminal_returns = compute_delisting_terminal_returns(
        observations, events, policy_dim, source=options.source, run_id=options.run_id
    )

    with store.transaction():
        store.con.execute("DELETE FROM delisting_terminal_returns WHERE source = ?", [options.source])
        if not terminal_returns.empty:
            insert_frame(
                store, terminal_returns, "delisting_terminal_returns", "delisting_terminal_returns_insert"
            )

    return int(len(terminal_returns))


def reconcile_delisting_codes(
    store: DuckDBStore,
    options: DelistingCodeReconciliationOptions | None = None,
) -> int:
    """Materialize ``delisting_code_reconciliation`` from the landed
    ``delisting_events``/``delisting_terminal_returns``/``delist_code_dim`` tables via
    :func:`compute_delisting_code_reconciliation`, replacing prior rows by source. Reports
    only; never rewrites the warehouse ``delist_code``.
    """

    options = options or DelistingCodeReconciliationOptions()
    store.initialize()

    events = store.con.execute(
        """
        SELECT delisting_event_id, security_id, symbol, delist_date, as_of_date, available_at, delist_code
        FROM delisting_events
        """
    ).df()
    terminal_returns = store.con.execute(
        """
        SELECT return_observation_id, security_id, symbol, delist_date, as_of_date, available_at, crsp_dlstcd
        FROM delisting_terminal_returns
        """
    ).df()
    code_dim = store.con.execute("SELECT delist_code, reason_category FROM delist_code_dim").df()

    reconciliation = compute_delisting_code_reconciliation(
        events, terminal_returns, code_dim, source=options.source, run_id=options.run_id
    )

    with store.transaction():
        store.con.execute("DELETE FROM delisting_code_reconciliation WHERE source = ?", [options.source])
        if not reconciliation.empty:
            insert_frame(
                store, reconciliation, "delisting_code_reconciliation", "delisting_code_reconciliation_insert"
            )

    return int(len(reconciliation))


class DelistingReturnObservationDataset(Dataset):
    dataset_id = "delisting_return_observations"
    source_name = "Injectable observed delisting returns"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: DelistingReturnObservationOptions,
    ) -> DatasetLoadResult:
        rows = load_delisting_return_observations(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="delisting_return_observations",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "provider": options.provider,
                "source_file": str(options.source_file) if options.source_file else None,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "provider": options.provider,
                "source_file": str(options.source_file) if options.source_file else None,
            },
        )
