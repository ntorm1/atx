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
