from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .signal_eval import IC_HORIZONS
from .warehouse import (
    file_sha256,
    insert_frame,
    json_dumps,
    now_utc_naive,
    quality_check,
    record_source_file,
    snake_case,
    symbol_key,
)


SOURCE_NAME = "ATX public delisting proxy builder"
DEFAULT_SOURCE = "atx_delisting_proxy_v1"
DEFAULT_CODE_SOURCE = "atx_delist_code_dim_v1"

# Shumway (1997), "The Delisting Bias in CRSP Data", Journal of Finance 52(1): the mean
# delisting return for NYSE/AMEX performance-related delistings is about -30%. Shumway &
# Warther (1999), JF 54(6), estimate about -55% for Nasdaq. These are named conventions, not
# observations. S4 preflight ruling (program.md): the convention is ON by default --
# DelistingTerminalReturnOptions.performance_delisting_return defaults to a
# ShumwayPerformancePolicy() instance -- and an operator opts OUT by passing
# performance_delisting_return=None. Dispatch between the two magnitudes is per-security,
# resolved from the event's own PIT listing exchange as of delist_date: a row
# resolved to Nasdaq is stamped terminal_return_policy='performance_unknown_nasdaq'; anything
# else (NYSE/AMEX/ARCA/BATS, or an unresolved exchange) is stamped
# terminal_return_policy='performance_unknown'. Either way a consumer can always filter
# imputed rows back out (`terminal_return_policy NOT LIKE 'performance_unknown%'`), and the
# coverage gate in quality/checks_survivorship.py makes an unset/uncovered gap loud either way.
SHUMWAY_PERFORMANCE_DELISTING_RETURN = -0.30
SHUMWAY_NASDAQ_PERFORMANCE_DELISTING_RETURN = -0.55
PERFORMANCE_TERMINAL_RETURN_POLICY_CODE = "performance_unknown"
PERFORMANCE_TERMINAL_RETURN_POLICY_CODE_NASDAQ = "performance_unknown_nasdaq"
PERFORMANCE_DELIST_REASONS = frozenset({"bankruptcy", "exchange_delist", "unknown"})

# Exchange tokens that count as "Nasdaq" for Shumway-variant dispatch, spanning both
# representations this warehouse actually produces: universe_us_listed_membership.exchange_code
# (MIC-style, from universe_us_listed.EXCHANGE_CODE_BY_DIRECTORY_EXCHANGE) writes "XNAS", while
# nasdaq_symbol_directory.exchange writes the literal "NASDAQ" for nasdaqlisted.txt rows.
# Comparison is case-insensitive (see _is_nasdaq_exchange).
NASDAQ_EXCHANGE_CODES = frozenset({"XNAS", "NASDAQ"})


@dataclass(frozen=True)
class ShumwayPerformancePolicy:
    """Per-exchange Shumway performance-delisting convention magnitudes.

    ``default_return`` (Shumway 1997) applies to a performance-related delist whose resolved
    listing exchange is NYSE/AMEX/ARCA/BATS or could not be resolved at all.
    ``nasdaq_return`` (Shumway & Warther 1999) applies only when the resolved exchange is
    Nasdaq. See :func:`apply_performance_delisting_policy` for the dispatch and
    :data:`NASDAQ_EXCHANGE_CODES` for the recognized Nasdaq tokens.

    This object is what :data:`DelistingTerminalReturnOptions.performance_delisting_return`
    carries when the convention is on; passing ``None`` there still opts out of the whole
    convention (no exchange resolution, no policy row, ever).
    """

    default_return: float = SHUMWAY_PERFORMANCE_DELISTING_RETURN
    nasdaq_return: float = SHUMWAY_NASDAQ_PERFORMANCE_DELISTING_RETURN


def _is_nasdaq_exchange(value: object) -> bool:
    return isinstance(value, str) and value.strip().upper() in NASDAQ_EXCHANGE_CODES


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
        False,
        None,
        "not_allowed_without_performance_related_reason",
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
    (
        "SEC_FORM_25",
        "ATX_PUBLIC_EVIDENCE",
        None,
        None,
        "SEC_FORM_25_NOTIFICATION",
        "exchange_delist",
        (
            "SEC Form 25 / 25-NSE notification of removal from listing and registration. "
            "25-NSE is exchange-initiated; a bare 25 is issuer-initiated."
        ),
        "DELISTED_FORM_25",
        True,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
        "performance_related_when_no_merger_evidence",
        DEFAULT_CODE_SOURCE,
    ),
    (
        "SEC_FORM_15",
        "ATX_PUBLIC_EVIDENCE",
        None,
        None,
        "SEC_FORM_15_DEREGISTRATION",
        "voluntary",
        (
            "SEC Form 15 deregistration / suspension of the duty to file. A voluntary exit "
            "from reporting, not a performance delisting."
        ),
        "DEREGISTERED_FORM_15",
        False,
        None,
        "not_allowed_voluntary_deregistration",
        DEFAULT_CODE_SOURCE,
    ),
    (
        "ARCHIVE_LAST_TRADE",
        "ATX_PUBLIC_EVIDENCE",
        None,
        None,
        "ARCHIVE_TRADING_CEASED",
        "dropped",
        (
            "Trading ceased in the ticker-history archive more than the configured session gap "
            "before the archive end, with no later bar. Lowest-confidence evidence."
        ),
        "NO_LONGER_TRADING_IN_ARCHIVE",
        True,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
        "performance_related_unknown_reason",
        DEFAULT_CODE_SOURCE,
    ),
    (
        "NASDAQ_FINANCIAL_STATUS_BANKRUPT",
        "ATX_PUBLIC_EVIDENCE",
        None,
        None,
        "NASDAQ_FINANCIAL_STATUS_Q",
        "bankruptcy",
        ("The Nasdaq symbol-directory financial_status carried the bankruptcy flag (Q) on or before the delist date."),
        "BANKRUPT",
        True,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
        "performance_related_bankruptcy",
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

    now = now_utc_naive()
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
# PF4-S4 S4-0/S4-1: observed + policy terminal-return catalog and DLSTCD reconciliation.
#
# delisting_return_observations (above) holds the raw vendor DLRET rows. This section
# collapses them to exactly one terminal return per delisted (security_id, delist_date) that
# later PF4-S4 tasks stitch into the forward-return series, and reconciles (reports, never
# overwrites) the vendor DLSTCD against the warehouse's own public delist_code proxy.
# terminal_return_source is 'observed' for a row backed by a real vendor DLRET and 'policy' for
# a row derived deterministically from a corporate action via terminal_return_policy_dim (S4-1).
# Observed always wins over policy for the same (security_id, delist_date). 'imputed' is never
# written to delisting_terminal_returns.
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
    # S4 T4 fix round 1: the four reason_category values the public-evidence delist_code_dim
    # rows carry (delisting.py's DELIST_CODE_ROWS, fed from delisting_evidence.py's precedence
    # fold). Each is a coarse, best-effort mapping to the CRSP DLSTCD family bucket a genuine
    # vendor record of the same event would most likely carry -- not a claim that the public
    # proxy alone can distinguish sub-reasons within a family.
    "exchange_delist": frozenset({"exchange", "dropped"}),  # SEC Form 25 / 25-NSE
    "voluntary": frozenset({"dropped"}),  # SEC Form 15 deregistration
    "dropped": frozenset({"dropped"}),  # archive last-trade inference (lowest confidence)
    "bankruptcy": frozenset({"dropped", "liquidation"}),  # bankruptcy or liquidation
}

# ---------------------------------------------------------------------------
# PF4-S4 S4-1: deterministic spinoff/merger terminal-return policy.
#
# terminal_return_policy_dim is policy-as-data: it is seeded once, idempotently, by migration
# 0185 (INSERT OR REPLACE over this exact tuple -- see db/migrations/bodies_0185_0188.py). There
# is deliberately no runtime seed_terminal_return_policy_dim() helper; the migration is the
# single source of truth and load_terminal_return_policy_dim() below is a read-only accessor.
#
# policy_code, corporate_action_type, terminal_return_basis, combine_successor, default_return,
# is_observed_required, description
TERMINAL_RETURN_POLICY_ROWS = (
    (
        "merger_cash",
        "merger",
        "cash_consideration",
        False,
        None,
        False,
        "Cash-merger consideration vs last pre-delist adjusted close = realized terminal return.",
    ),
    (
        "merger_stock",
        "merger",
        "successor_reinvest",
        True,
        None,
        False,
        "Stock-merger: proceeds reinvested into the successor security_id; terminal return "
        "chains to the successor path.",
    ),
    (
        "spinoff",
        "spinoff",
        "parent_plus_child",
        True,
        None,
        False,
        "Spinoff: parent close plus when-issued child value; combined via successor_security_id.",
    ),
    (
        "liquidation",
        "liquidation",
        "final_distribution",
        False,
        None,
        True,
        "Liquidation: observed final cash distribution required; no default.",
    ),
    (
        "exchange_delete",
        "exchange_delete",
        "observed_dlret",
        False,
        None,
        True,
        "Exchange delete: observed DLRET required; no policy default (public proxy stays "
        "UNOBSERVED).",
    ),
    (
        "dropped_unresolved",
        "dropped",
        "unresolved",
        False,
        None,
        True,
        "Unresolved drop: no policy terminal return; handled only if an observed/imputed value "
        "is supplied elsewhere.",
    ),
    (
        "performance_unknown",
        "performance_delist",
        "shumway_default",
        False,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
        False,
        "Performance-related delisting with no observed DLRET, resolved (PIT, as of "
        "delist_date) to a NYSE/AMEX/ARCA/BATS listing or to no resolvable exchange at all: "
        "apply the documented Shumway (1997) -30% convention. On by default (S4 preflight "
        "ruling); an operator opts out by setting "
        "DelistingTerminalReturnOptions.performance_delisting_return=None.",
    ),
    (
        "performance_unknown_nasdaq",
        "performance_delist",
        "shumway_nasdaq_default",
        False,
        SHUMWAY_NASDAQ_PERFORMANCE_DELISTING_RETURN,
        False,
        "Performance-related delisting with no observed DLRET, resolved (PIT, as of "
        "delist_date) to a Nasdaq listing: apply the documented Shumway & Warther (1999) "
        "-55% convention. Same on-by-default / opt-out-via-None behavior as "
        "performance_unknown.",
    ),
)

POLICY_DIM_COLUMNS = [
    "policy_code",
    "corporate_action_type",
    "terminal_return_basis",
    "combine_successor",
    "default_return",
    "is_observed_required",
    "description",
]

# apply_terminal_return_policy's own output shape -- compute_delisting_terminal_returns adds
# source/run_id/terminal_return_id when it unions these rows with the observed ones.
POLICY_TERMINAL_RETURN_COLUMNS = [
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
]

# The basis -> (required inputs, formula) contract. This dict is the ONLY place a
# terminal_return_basis name appears in code -- policy selection (see apply_terminal_return_policy)
# never branches on policy_code, only on whether a candidate's basis has its required inputs.
# final_distribution / observed_dlret / unresolved are intentionally absent: every
# TERMINAL_RETURN_POLICY_ROWS entry using one of those bases has is_observed_required=True and is
# filtered out before this contract is ever consulted (R2 step 2).
_TERMINAL_RETURN_BASIS_CONTRACT: dict[str, tuple[tuple[str, ...], Callable[[pd.Series], float]]] = {
    "cash_consideration": (
        ("cash_amount", "last_pre_delist_adjusted_close"),
        lambda row: float(row["cash_amount"]) / float(row["last_pre_delist_adjusted_close"]) - 1.0,
    ),
    "successor_reinvest": (
        ("successor_security_id", "successor_value", "last_pre_delist_adjusted_close"),
        lambda row: float(row["successor_value"]) / float(row["last_pre_delist_adjusted_close"]) - 1.0,
    ),
    "parent_plus_child": (
        ("parent_value", "child_value", "last_pre_delist_adjusted_close"),
        lambda row: (
            (float(row["parent_value"]) + float(row["child_value"]))
            / float(row["last_pre_delist_adjusted_close"])
            - 1.0
        ),
    ),
}


def _empty_policy_terminal_return_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=POLICY_TERMINAL_RETURN_COLUMNS)


def load_terminal_return_policy_dim(store: DuckDBStore) -> pd.DataFrame:
    """Read the seeded, policy-as-data ``terminal_return_policy_dim`` dimension.

    The dimension is seeded once, idempotently, by migration 0185 (``TERMINAL_RETURN_POLICY_ROWS``).
    This is a read-only accessor -- there is no runtime seeder to mirror ``seed_delist_code_dim``.
    """

    store.initialize()
    return store.con.execute(
        f"SELECT {', '.join(POLICY_DIM_COLUMNS)} FROM terminal_return_policy_dim"
    ).df()


def apply_terminal_return_policy(
    events: pd.DataFrame,
    corporate_actions: pd.DataFrame,
    policy_dim: pd.DataFrame,
) -> pd.DataFrame:
    """Deterministically derive a policy terminal return for delists carrying no observed DLRET.

    ``events`` rows must already carry a ``corporate_action_type`` column (the same vocabulary as
    ``terminal_return_policy_dim.corporate_action_type``); the caller is responsible for attaching
    it (see :func:`compute_delisting_terminal_returns`). ``corporate_actions`` supplies the
    per-event financial inputs the matched policy's basis needs (``cash_amount``,
    ``successor_security_id``/``successor_value``, ``parent_value``/``child_value``,
    ``last_pre_delist_adjusted_close``, ``last_pre_delist_available_at``, ``available_at``),
    joined on ``(security_id, delist_date == ex_date)``.

    Selection (S4-1 brief R2): candidates are the ``policy_dim`` rows whose
    ``corporate_action_type`` matches the event, discarding any candidate with
    ``is_observed_required`` true. The remaining candidates are evaluated in ascending
    ``policy_code`` order; the first whose ``terminal_return_basis`` has every required input
    present (and, uniformly, a strictly positive ``last_pre_delist_adjusted_close``) wins. No
    candidate qualifying means no row for that event -- this function never invents a return.

    ``available_at`` (S4-1 brief R3) is ``max(corporate_action.available_at,
    last_pre_delist_available_at)``, never a fallback to ``delist_date``/``as_of_date``/``now()``;
    a candidate whose corporate action carries no ``available_at`` at all is skipped rather than
    guessing one.

    Pure, stable-sorted: same inputs in any row order produce byte-identical output. Exactly one
    row is ever emitted per ``(security_id, delist_date)``.
    """

    if events.empty or corporate_actions.empty or policy_dim.empty:
        return _empty_policy_terminal_return_frame()
    if "corporate_action_type" not in events.columns:
        return _empty_policy_terminal_return_frame()

    ev = events.copy().reset_index(drop=True)

    actions = corporate_actions.copy().reset_index(drop=True)
    if "ex_date" in actions.columns and "delist_date" not in actions.columns:
        actions = actions.rename(columns={"ex_date": "delist_date"})
    actions = actions.drop(columns=[c for c in ("symbol",) if c in actions.columns])

    required_join_columns = {"security_id", "delist_date"}
    if not required_join_columns.issubset(ev.columns) or not required_join_columns.issubset(actions.columns):
        return _empty_policy_terminal_return_frame()

    # A DuckDB-sourced frame's delist_date is datetime64; a hand-built (e.g. test) frame's is
    # often plain datetime.date -- pandas.merge raises on that dtype mismatch rather than
    # coercing it. Join on a normalized *copy* of the key so callers can freely mix either
    # representation; the delist_date retained in the output is always ev's own original value.
    ev = ev.assign(_delist_date_key=pd.to_datetime(ev["delist_date"], errors="coerce"))
    actions = actions.assign(_delist_date_key=pd.to_datetime(actions["delist_date"], errors="coerce")).drop(
        columns=["delist_date"]
    )

    merged = ev.merge(actions, on=["security_id", "_delist_date_key"], how="inner", suffixes=("", "_action"))
    merged = merged.drop(columns=["_delist_date_key"])
    if merged.empty:
        return _empty_policy_terminal_return_frame()

    # Deterministic candidate order: is_observed_required policies never yield a policy row
    # (R2 step 2), and the survivors are walked in ascending policy_code order (R2 step 3).
    candidates = policy_dim[~policy_dim["is_observed_required"].astype(bool)].copy()
    candidates = candidates.sort_values(by="policy_code", kind="mergesort").reset_index(drop=True)

    # Stable-sorted event iteration so output does not depend on input row order.
    order_keys = [c for c in ("security_id", "delist_date", "symbol") if c in merged.columns]
    merged = merged.sort_values(by=order_keys, kind="mergesort", na_position="last").reset_index(drop=True)

    rows: list[dict[str, object]] = []
    emitted_keys: set[tuple[object, object]] = set()
    for _, event_row in merged.iterrows():
        key = (event_row.get("security_id"), event_row.get("delist_date"))
        if key in emitted_keys:
            continue  # a duplicate event/action match must still yield at most one row
        action_type = event_row.get("corporate_action_type")
        if pd.isna(action_type):
            continue
        available_at = _coalesce_later(
            event_row.get("available_at_action")
            if "available_at_action" in event_row.index
            else event_row.get("available_at"),
            event_row.get("last_pre_delist_available_at"),
        )
        if pd.isna(available_at):
            continue  # no-lookahead: never fall back to delist_date/as_of_date/now()

        eligible = candidates[candidates["corporate_action_type"] == action_type]
        for _, policy_row in eligible.iterrows():
            basis = policy_row["terminal_return_basis"]
            contract = _TERMINAL_RETURN_BASIS_CONTRACT.get(basis)
            if contract is None:
                continue
            required_inputs, formula = contract
            if not all(col in event_row.index and pd.notna(event_row[col]) for col in required_inputs):
                continue
            last_close = event_row.get("last_pre_delist_adjusted_close")
            if pd.isna(last_close) or float(last_close) <= 0:
                continue
            terminal_return = formula(event_row)
            if not math.isfinite(terminal_return):
                continue

            rows.append(
                {
                    "security_id": event_row.get("security_id"),
                    "symbol": event_row.get("symbol"),
                    "delist_date": event_row.get("delist_date"),
                    "as_of_date": event_row.get("as_of_date"),
                    "available_at": available_at,
                    "terminal_return": terminal_return,
                    "terminal_return_ex_div": pd.NA,
                    "terminal_return_source": "policy",
                    "terminal_return_policy": policy_row["policy_code"],
                    "crsp_dlstcd": pd.NA,
                    "return_basis": basis,
                    "successor_security_id": event_row.get("successor_security_id"),
                    "return_observation_id": pd.NA,
                }
            )
            emitted_keys.add(key)
            break  # first qualifying candidate wins (R2 step 3)

    if not rows:
        return _empty_policy_terminal_return_frame()

    result = pd.DataFrame(rows)
    result = result.sort_values(
        by=[c for c in ("security_id", "delist_date", "symbol") if c in result.columns],
        kind="mergesort",
        na_position="last",
    ).reset_index(drop=True)
    return result[POLICY_TERMINAL_RETURN_COLUMNS]


def apply_performance_delisting_policy(
    events: pd.DataFrame,
    policy_dim: pd.DataFrame,
    *,
    performance_return: ShumwayPerformancePolicy | float | None,
    reasons: frozenset[str] = PERFORMANCE_DELIST_REASONS,
) -> pd.DataFrame:
    """Apply the documented Shumway convention to performance-related delists.

    ``events`` carries ``security_id, symbol, delist_date, as_of_date, available_at,
    delist_reason`` (the ``delisting_events`` shape) and optionally
    ``successor_security_id, listing_exchange_code, listing_exchange_available_at``.
    A row qualifies when its ``delist_reason`` is in ``reasons``
    AND ``performance_return`` is not None AND the event carries a real ``available_at`` --
    this function never invents a timestamp, exactly like :func:`apply_terminal_return_policy`.

    Returns a :data:`POLICY_TERMINAL_RETURN_COLUMNS` frame with
    ``terminal_return_source='policy'`` and the exchange-specific policy code and basis.
    The policy object dispatches Nasdaq separately; a float retains the explicit scalar
    override. Exchange inputs must be PIT-resolved by the caller, as the refresh path does.
    Availability is the maximum of the event and the selected exchange input.
    Pure, stable-sorted, and empty whenever
    ``performance_return`` is None -- callers opt out by passing None.
    """

    if performance_return is None:
        return _empty_policy_terminal_return_frame()
    if events is None or events.empty or policy_dim is None or policy_dim.empty:
        return _empty_policy_terminal_return_frame()
    if "delist_reason" not in events.columns:
        return _empty_policy_terminal_return_frame()
    frame = events.copy().reset_index(drop=True)
    eligible = frame["delist_reason"].astype("string").isin(sorted(reasons))
    eligible &= pd.to_datetime(frame["available_at"], errors="coerce").notna()
    frame = frame[eligible].copy()
    if frame.empty:
        return _empty_policy_terminal_return_frame()

    frame["terminal_return_policy"] = PERFORMANCE_TERMINAL_RETURN_POLICY_CODE
    if isinstance(performance_return, ShumwayPerformancePolicy):
        frame["terminal_return"] = performance_return.default_return
        if "listing_exchange_code" in frame.columns:
            nasdaq = frame["listing_exchange_code"].map(_is_nasdaq_exchange)
            frame.loc[nasdaq, "terminal_return"] = performance_return.nasdaq_return
            frame.loc[nasdaq, "terminal_return_policy"] = PERFORMANCE_TERMINAL_RETURN_POLICY_CODE_NASDAQ
        if "listing_exchange_available_at" in frame.columns:
            frame["available_at"] = pd.concat(
                [
                    pd.to_datetime(frame["available_at"], errors="coerce"),
                    pd.to_datetime(frame["listing_exchange_available_at"], errors="coerce"),
                ],
                axis=1,
            ).max(axis=1)
    else:
        frame["terminal_return"] = float(performance_return)
    bases = policy_dim.set_index("policy_code")["terminal_return_basis"]
    frame["return_basis"] = frame["terminal_return_policy"].map(bases)
    frame = frame[frame["return_basis"].notna()]
    if frame.empty:
        return _empty_policy_terminal_return_frame()

    out = pd.DataFrame(
        {
            "security_id": frame["security_id"],
            "symbol": frame.get("symbol"),
            "delist_date": frame["delist_date"],
            "as_of_date": frame.get("as_of_date"),
            "available_at": pd.to_datetime(frame["available_at"]),
            "terminal_return": frame["terminal_return"],
            "terminal_return_ex_div": pd.NA,
            "terminal_return_source": "policy",
            "terminal_return_policy": frame["terminal_return_policy"],
            "crsp_dlstcd": pd.NA,
            "return_basis": frame["return_basis"],
            "successor_security_id": frame.get("successor_security_id"),
            "return_observation_id": pd.NA,
        }
    )
    out = out.drop_duplicates(subset=["security_id", "delist_date"], keep="first")
    return out.sort_values(["security_id", "delist_date"], kind="mergesort", na_position="last").reset_index(
        drop=True
    )[POLICY_TERMINAL_RETURN_COLUMNS]


@dataclass(frozen=True)
class DelistingTerminalReturnOptions:
    source: str = DEFAULT_TERMINAL_RETURN_SOURCE
    run_id: str | None = None
    # ON by default, dispatched per historical exchange; None opts out. A scalar remains
    # an explicit operator override for compatibility with the original policy interface.
    performance_delisting_return: ShumwayPerformancePolicy | float | None = ShumwayPerformancePolicy()


@dataclass(frozen=True)
class DelistingCodeReconciliationOptions:
    source: str = DEFAULT_RECONCILIATION_SOURCE
    run_id: str | None = None


def _empty_terminal_return_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=TERMINAL_RETURN_COLUMNS)


def _concat_terminal_return_frames(frames: list[pd.DataFrame]) -> pd.DataFrame:
    """Union the observed and policy terminal-return frames without tripping pandas 2.2's
    all-NA/empty concat ``FutureWarning`` (S4-1-fix M1).

    Empty pieces are dropped, and any column that is entirely NA in *some* remaining piece is
    coerced to ``object`` in *every* piece before the concat. Without this, pandas excludes the
    all-NA piece from result-dtype inference and warns that the dtype will change in a future
    version -- the concrete trigger here is a float64 observed ``terminal_return_ex_div`` (a real
    DLRETX value) unioned with an all-NA policy ``terminal_return_ex_div``. Object-aligning the
    affected columns makes the exclusion a no-op, so the union is warning-clean and safe to insert
    into DuckDB (which coerces each object column back to its declared type). Non-conflicting
    columns keep their dtypes.
    """

    frames = [frame for frame in frames if not frame.empty]
    if not frames:
        return _empty_terminal_return_frame()
    all_na_columns = {column for frame in frames for column in frame.columns if frame[column].isna().all()}
    if all_na_columns:
        frames = [
            frame.astype({column: object for column in all_na_columns if column in frame.columns})
            for frame in frames
        ]
    return pd.concat(frames, ignore_index=True)


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
    corporate_actions: pd.DataFrame | None = None,
    performance_delisting_return: ShumwayPerformancePolicy | float | None = None,
) -> pd.DataFrame:
    """Collapse ``delisting_return_observations`` to one terminal return per
    ``(security_id, delist_date)``, then fill remaining coverage gaps from (S4-1) the
    deterministic corporate-action policy and (S4-4) the opt-in Shumway performance-delisting
    policy -- in that priority order, so observed always wins over corporate-action policy,
    which always wins over the performance convention, for the same ``(security_id,
    delist_date)``.

    The latest-visible observation wins: ``ORDER BY available_at DESC, source_loaded_at
    DESC, delisting_return_observation_id DESC`` -- the same tie-break
    ``DELISTING_EVENTS_ASOF_SQL``'s ``observation_candidates`` CTE already uses (see
    ``db/asof/security.py``). Every observed row is tagged ``terminal_return_source='observed'``.
    ``imputed`` is never written here.

    ``corporate_actions`` is keyword-only and defaults to ``None``: with it omitted, the
    corporate-action policy path is skipped. When supplied (and non-empty), :func:`
    apply_terminal_return_policy` is called against exactly the events that have **no** observed
    terminal return for their ``(security_id, delist_date)``. ``events`` rows carry no
    ``corporate_action_type`` of their own (that column does not exist on ``delisting_events``);
    it is attached here from ``corporate_actions.action_type``, joined on ``(security_id,
    delist_date == ex_date)`` -- an event with no matching corporate action simply gets no policy
    row, it is never invented.

    ``performance_delisting_return`` (S4-4) is independent of ``corporate_actions`` -- a
    public-evidence-only warehouse has no licensed corporate-action feed at all, which is
    exactly the profile the Shumway convention exists for. When not None, :func:`
    apply_performance_delisting_policy` is called against the events still uncovered after the
    observed and corporate-action-policy passes, using ``events.delist_reason``. Passing None
    opts out entirely (no row is ever invented).

    ``available_at`` is inherited verbatim from the observation's ``available_at`` (the
    delisting-confirmation timestamp) for observed rows, and is ``max(corporate action
    available_at, last pre-delist bar available_at)`` for corporate-action-policy rows, and the
    maximum event/exchange ``available_at`` for performance-policy rows -- never the delist event date in
    any case: the no-lookahead invariant the survivorship fix depends on.
    """

    if observations.empty:
        result = _empty_terminal_return_frame()
    else:
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
            result = _empty_terminal_return_frame()
        else:
            if "source_loaded_at" not in obs.columns:
                obs["source_loaded_at"] = pd.NaT

            # Stable collapse: latest-visible observation wins per (security_id, delist_date).
            # kind="mergesort" is a stable sort so ties beyond the explicit tie-break columns
            # still resolve deterministically -- required for "same inputs -> byte-identical rows".
            obs = obs.sort_values(
                by=[
                    "security_id", "delist_date", "available_at", "source_loaded_at",
                    "delisting_return_observation_id",
                ],
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
                    "as_of_date": (
                        winners["as_of_date"] if "as_of_date" in winners.columns else winners["delist_date"]
                    ),
                    "available_at": winners["available_at"],
                    "terminal_return": winners["delisting_return"],
                    "terminal_return_ex_div": (
                        winners["delisting_return_ex_div"]
                        if "delisting_return_ex_div" in winners.columns
                        else pd.NA
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
            result = result[TERMINAL_RETURN_COLUMNS]

    events_ok = not events.empty and "security_id" in events.columns and "delist_date" in events.columns
    can_apply_corporate_action_policy = (
        events_ok and corporate_actions is not None and not corporate_actions.empty and not policy_dim.empty
    )
    can_apply_performance_policy = (
        events_ok
        and performance_delisting_return is not None
        and not policy_dim.empty
        and "delist_reason" in events.columns
    )
    if not can_apply_corporate_action_policy and not can_apply_performance_policy:
        return result

    def _covered_pairs(frame: pd.DataFrame) -> pd.DataFrame:
        # A DuckDB-sourced frame's delist_date is datetime64; a hand-built (e.g. test) frame's is
        # often plain datetime.date -- normalize both sides to the same _delist_date_key so the
        # merge below never raises on a dtype mismatch.
        if frame.empty:
            return pd.DataFrame(columns=["security_id", "_delist_date_key", "_covered"])
        return (
            frame[["security_id", "delist_date"]]
            .assign(_delist_date_key=pd.to_datetime(frame["delist_date"], errors="coerce"))
            .drop(columns=["delist_date"])
            .drop_duplicates()
            .assign(_covered=True)
        )

    policy_result = _empty_terminal_return_frame()
    if can_apply_corporate_action_policy:
        ev = events.copy().reset_index(drop=True)
        ev = ev.assign(_delist_date_key=pd.to_datetime(ev["delist_date"], errors="coerce"))
        observed_pairs = _covered_pairs(result)
        if not observed_pairs.empty:
            ev = ev.merge(observed_pairs, on=["security_id", "_delist_date_key"], how="left")
            uncovered = ev[ev["_covered"].isna()].drop(columns=["_covered"]).reset_index(drop=True)
        else:
            uncovered = ev

        if not uncovered.empty:
            ca = corporate_actions.copy().reset_index(drop=True)
            if "ex_date" in ca.columns and "delist_date" not in ca.columns:
                ca = ca.rename(columns={"ex_date": "delist_date"})

            if "corporate_action_type" not in uncovered.columns:
                if "action_type" in ca.columns and {"security_id", "delist_date"}.issubset(ca.columns):
                    type_lookup = (
                        ca[["security_id", "delist_date", "action_type"]]
                        .dropna(subset=["security_id", "delist_date"])
                        .assign(_delist_date_key=lambda frame: pd.to_datetime(frame["delist_date"], errors="coerce"))
                        .drop(columns=["delist_date"])
                        .drop_duplicates(subset=["security_id", "_delist_date_key"])
                        .rename(columns={"action_type": "corporate_action_type"})
                    )
                    uncovered = uncovered.merge(type_lookup, on=["security_id", "_delist_date_key"], how="left")
                else:
                    uncovered = uncovered.assign(corporate_action_type=pd.NA)

            uncovered = uncovered.drop(columns=["_delist_date_key"])

            policy_rows = apply_terminal_return_policy(uncovered, corporate_actions, policy_dim)
            if not policy_rows.empty:
                policy_result = pd.DataFrame(
                    {
                        "source": source,
                        "security_id": policy_rows["security_id"],
                        "symbol": policy_rows["symbol"],
                        "delist_date": policy_rows["delist_date"],
                        "as_of_date": policy_rows["as_of_date"],
                        "available_at": policy_rows["available_at"],
                        "terminal_return": policy_rows["terminal_return"],
                        "terminal_return_ex_div": policy_rows["terminal_return_ex_div"],
                        "terminal_return_source": policy_rows["terminal_return_source"],
                        "terminal_return_policy": policy_rows["terminal_return_policy"],
                        "crsp_dlstcd": policy_rows["crsp_dlstcd"],
                        "return_basis": policy_rows["return_basis"],
                        "successor_security_id": policy_rows["successor_security_id"],
                        "return_observation_id": policy_rows["return_observation_id"],
                        "run_id": run_id,
                    }
                )
                policy_result["terminal_return_id"] = policy_result.apply(_stable_terminal_return_id, axis=1)
                policy_result = policy_result[TERMINAL_RETURN_COLUMNS]

    performance_result = _empty_terminal_return_frame()
    if can_apply_performance_policy:
        ev2 = events.copy().reset_index(drop=True)
        ev2 = ev2.assign(_delist_date_key=pd.to_datetime(ev2["delist_date"], errors="coerce"))
        # Filter out empty pieces before concatenating (mirrors _concat_terminal_return_frames):
        # an empty _covered_pairs frame has no real dtype for _delist_date_key, and concatenating
        # it with a non-empty datetime64 piece can silently upcast the result to object, which
        # then raises a merge dtype error against ev2's datetime64 _delist_date_key below.
        covered_parts = [p for p in (_covered_pairs(result), _covered_pairs(policy_result)) if not p.empty]
        already_covered = pd.concat(covered_parts, ignore_index=True) if covered_parts else pd.DataFrame()
        if not already_covered.empty:
            already_covered = already_covered.drop_duplicates(subset=["security_id", "_delist_date_key"])
            ev2 = ev2.merge(already_covered, on=["security_id", "_delist_date_key"], how="left")
            still_uncovered = (
                ev2[ev2["_covered"].isna()].drop(columns=["_covered", "_delist_date_key"]).reset_index(drop=True)
            )
        else:
            still_uncovered = ev2.drop(columns=["_delist_date_key"])

        performance_rows = apply_performance_delisting_policy(
            still_uncovered, policy_dim, performance_return=performance_delisting_return
        )
        if not performance_rows.empty:
            performance_result = pd.DataFrame(
                {
                    "source": source,
                    "security_id": performance_rows["security_id"],
                    "symbol": performance_rows["symbol"],
                    "delist_date": performance_rows["delist_date"],
                    "as_of_date": performance_rows["as_of_date"],
                    "available_at": performance_rows["available_at"],
                    "terminal_return": performance_rows["terminal_return"],
                    "terminal_return_ex_div": performance_rows["terminal_return_ex_div"],
                    "terminal_return_source": performance_rows["terminal_return_source"],
                    "terminal_return_policy": performance_rows["terminal_return_policy"],
                    "crsp_dlstcd": performance_rows["crsp_dlstcd"],
                    "return_basis": performance_rows["return_basis"],
                    "successor_security_id": performance_rows["successor_security_id"],
                    "return_observation_id": performance_rows["return_observation_id"],
                    "run_id": run_id,
                }
            )
            performance_result["terminal_return_id"] = performance_result.apply(_stable_terminal_return_id, axis=1)
            performance_result = performance_result[TERMINAL_RETURN_COLUMNS]

    combined = _concat_terminal_return_frames([result, policy_result, performance_result])
    if combined.empty:
        return result
    # Sort on a normalized copy of delist_date, not the column itself: result's delist_date
    # (observations-sourced) and policy_result's/performance_result's (events-sourced) can carry
    # different concrete date representations (e.g. one DuckDB-native, one a hand-built
    # datetime.date in a test), and pandas raises rather than coerces when comparing a Timestamp
    # to a plain date directly.
    sort_key = pd.to_datetime(combined["delist_date"], errors="coerce")
    combined = (
        combined.assign(_delist_date_sort_key=sort_key)
        .sort_values(
            by=["security_id", "_delist_date_sort_key", "terminal_return_id"],
            kind="mergesort",
            na_position="last",
        )
        .drop(columns=["_delist_date_sort_key"])
        .reset_index(drop=True)
    )
    return combined[TERMINAL_RETURN_COLUMNS]


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
    result["reconciliation_id"] = result.apply(_stable_reconciliation_id, axis=1)
    # reconciliation_id terminates the sort key so it is a total order: (security_id,
    # delist_date, symbol) alone ties whenever two delisting_events rows share a security-day
    # (e.g. a listing-status delete and a snapshot-absence event on the same day), and a stable
    # mergesort would then just preserve whatever order the input frames happened to arrive in --
    # violating "same inputs + params -> byte-identical rows" even though the row set is stable.
    result = result.sort_values(
        by=["security_id", "delist_date", "symbol", "reconciliation_id"], kind="mergesort", na_position="last"
    ).reset_index(drop=True)
    return result[RECONCILIATION_COLUMNS]


def _load_terminal_return_events(store: DuckDBStore, *, resolve_exchange: bool) -> pd.DataFrame:
    """Resolve the listing at delist-date close without borrowing future snapshots.

    Membership intervals precede exchange listings, then directory snapshots. Both economic
    dates and input availability are bounded by delist_date (22:00 close). Missing exchange
    metadata stays unresolved and uses the documented non-Nasdaq fallback. A latest-revision
    flag alone is insufficient: a current snapshot cannot classify a historical delisting.
    """

    event_sql = """
        SELECT delisting_event_id, security_id, symbol, delist_date, as_of_date, available_at,
               delist_code, delist_reason
        FROM delisting_events
    """
    if not resolve_exchange:
        return store.con.execute(event_sql + " ORDER BY delisting_event_id").df()
    return store.con.execute(
        f"""
        WITH events AS ({event_sql})
        SELECT e.*, x.exchange_code AS listing_exchange_code,
               x.available_at AS listing_exchange_available_at
        FROM events e
        LEFT JOIN LATERAL (
            SELECT exchange_code, available_at
            FROM (
                SELECT 1 AS priority, u.exchange_code, u.available_at, u.as_of_date,
                       u.valid_from, u.membership_id AS tie_break
                FROM universe_us_listed_membership u
                WHERE u.security_id = e.security_id
                  AND u.universe_id = 'us_listed_v1'
                  AND u.is_latest_revision
                  AND u.valid_from <= e.delist_date
                  AND (u.valid_to IS NULL OR u.valid_to >= e.delist_date)
                UNION ALL
                SELECT 2, coalesce(nullif(trim(l.exchange_code), ''), l.mic),
                       l.available_at, l.as_of_date, l.valid_from,
                       concat(l.source, '|', l.ticker, '|', l.mic)
                FROM exchange_listings l
                WHERE l.security_id = e.security_id
                  AND coalesce(l.is_latest_revision, true)
                  AND l.valid_from <= e.delist_date
                  AND (l.valid_to IS NULL OR l.valid_to > e.delist_date)
                UNION ALL
                SELECT 3,
                       CASE WHEN d.directory = 'nasdaqlisted' THEN 'XNAS' ELSE d.exchange END,
                       d.available_at, d.as_of_date, d.as_of_date,
                       concat(d.directory, '|', d.source_url)
                FROM nasdaq_symbol_directory d
                WHERE d.symbol = e.symbol
                  AND coalesce(d.is_latest_revision, true)
            ) candidates
            WHERE nullif(trim(exchange_code), '') IS NOT NULL
              AND as_of_date <= e.delist_date
              AND available_at <= e.delist_date + INTERVAL '22 hours'
            ORDER BY priority, as_of_date DESC, available_at DESC, valid_from DESC,
                     tie_break, exchange_code
            LIMIT 1
        ) x ON true
        ORDER BY e.delisting_event_id
        """
    ).df()


def refresh_delisting_terminal_returns(
    store: DuckDBStore,
    options: DelistingTerminalReturnOptions | None = None,
) -> int:
    """Materialize ``delisting_terminal_returns`` from the landed observation/event/policy/
    corporate-action tables via :func:`compute_delisting_terminal_returns`, replacing prior rows
    by source.

    S4-1: ``corporate_actions`` is read alongside its last pre-delist bar from
    ``equity_price_metrics`` -- the ``is_latest_revision``-filtered row with the greatest
    ``trade_date`` strictly less than the action's ``ex_date`` -- via an ``ASOF LEFT JOIN``, so a
    corporate action with no eligible prior bar simply carries a null
    ``last_pre_delist_adjusted_close`` (the policy applier then correctly emits no row for it,
    rather than the join silently dropping the corporate action). ``equity_price_metrics.
    available_at`` is ``NOT NULL`` (unlike ``equity_daily_bars.available_at``), which is why it
    -- not ``equity_daily_bars`` -- is the source here.
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
    events = _load_terminal_return_events(
        store, resolve_exchange=isinstance(options.performance_delisting_return, ShumwayPerformancePolicy)
    )
    policy_dim = store.con.execute(
        """
        SELECT
            policy_code, corporate_action_type, terminal_return_basis,
            combine_successor, default_return, is_observed_required, description
        FROM terminal_return_policy_dim
        """
    ).df()
    corporate_actions = store.con.execute(
        """
        SELECT
            ca.security_id,
            ca.action_type,
            ca.ex_date AS delist_date,
            ca.cash_amount,
            ca.available_at,
            epm.adjusted_close AS last_pre_delist_adjusted_close,
            epm.available_at AS last_pre_delist_available_at
        FROM corporate_actions ca
        ASOF LEFT JOIN (
            SELECT security_id, trade_date, adjusted_close, available_at
            FROM equity_price_metrics
            WHERE is_latest_revision = true
        ) epm
          ON ca.security_id = epm.security_id
         AND ca.ex_date > epm.trade_date
        """
    ).df()

    terminal_returns = compute_delisting_terminal_returns(
        observations,
        events,
        policy_dim,
        source=options.source,
        run_id=options.run_id,
        corporate_actions=corporate_actions,
        performance_delisting_return=options.performance_delisting_return,
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


# ===========================================================================
# PF4-S4 S4-2: survivorship-safe forward-return stitching.
#
# Production reads corrected same-row equity_daily_bars.adjusted_close. Raw close
# remains an explicit compatibility option. Neither the modeled bar availability
# nor the vendor cumulative adjustment factor proves historical economic/PIT quality.
# The calendar is the observed union of bar dates, not an official exchange calendar.
# All horizons count this SAME calendar for price endpoints, dates and DQC windows.
# ===========================================================================

RAW_FORWARD_RETURN_SOURCE = "equity_daily_bars"
DEFAULT_FORWARD_RETURN_SS_SOURCE = "atx_forward_returns_survivorship_safe_v1"

# Existing observed-session calendar, populated by TradingCalendarDataset.
_TRADING_CALENDAR_ID = "XNYS"
_TRADING_CALENDAR_SOURCE = "equity_daily_bars calendar"

FORWARD_RETURN_SS_COLUMNS = [
    "forward_return_id", "source", "security_id", "symbol", "as_of_date", "horizon_days",
    "forward_end_date", "raw_forward_return", "terminal_return", "forward_return",
    "is_delisted_in_horizon", "is_stitched", "delist_date", "terminal_return_source",
    "return_observation_id", "is_latest_revision", "available_at", "run_id",
]

# Canonical columns of the two inputs to compute_survivorship_safe_forward_returns. Used only to
# reindex an EMPTY input frame so the outer merge's join keys always exist on both sides (see the
# empty-frame guard inside the transform).
_FORWARD_RETURN_BASE_COLUMNS = [
    "security_id", "as_of_date", "horizon_days", "symbol", "forward_end_date",
    "raw_forward_return", "available_at",
]
_DELISTING_COHORT_COLUMNS = [
    "security_id", "as_of_date", "horizon_days", "delist_date", "terminal_return",
    "terminal_return_source", "return_observation_id", "terminal_available_at",
]


@dataclass(frozen=True)
class SurvivorshipSafeForwardReturnOptions:
    source: str = DEFAULT_FORWARD_RETURN_SS_SOURCE
    run_id: str | None = None
    # Production uses the source's corrected same-row adjusted price. This is not
    # certification of economic adjustment quality or historical source vintages.
    # Raw close is an explicit compatibility mode; NULL adjustments never fall back.
    price_basis: str = "adjusted_close"
    observation_cutoff: dt.datetime | None = None


def _stitch(raw, terminal):
    if terminal is None or pd.isna(terminal):
        return raw
    if raw is None or pd.isna(raw):
        return float(terminal)
    return float((1.0 + float(raw)) * (1.0 + float(terminal)) - 1.0)


def _forward_return_id(source, security_id, as_of_date, horizon_days):
    payload = "|".join(str(p) for p in (source, security_id, as_of_date, horizon_days))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def compute_survivorship_safe_forward_returns(
    forward_returns,
    delisting_cohort,
    *,
    source=DEFAULT_FORWARD_RETURN_SS_SOURCE,
    run_id=None,
):
    """Splice observed/policy terminal returns into the surviving forward-return panel so a name
    delisting inside a horizon window realizes its terminal DLRET instead of being NaN-dropped.

    ``forward_returns`` is the surviving panel (``security_id, symbol, as_of_date, horizon_days,
    forward_end_date, raw_forward_return, available_at``). ``delisting_cohort`` enumerates
    ``(security_id, as_of_date, horizon_days, delist_date, terminal_return, terminal_return_source,
    return_observation_id, terminal_available_at)`` for every panel formation date whose forward
    window ``(as_of_date, forward_end_date]`` contains a ``delist_date`` with a known terminal
    return. The transform geometrically splices the pre-delist partial return with the terminal
    DLRET (``(1+raw)*(1+terminal)-1``), or emits the bare terminal when no surviving bar exists
    (the pure NaN-drop case); it sets ``is_stitched``/``is_delisted_in_horizon`` and carries
    ``available_at = max(raw.available_at, terminal_available_at)`` so a stitched row is not
    consumable until the later of the two resolves (no lookahead). Non-cohort surviving rows pass
    through unchanged. Stable-sorted; ``forward_return_id`` is a deterministic hash of
    ``(source, security_id, as_of_date, horizon_days)``.
    """

    base = forward_returns.copy() if forward_returns is not None else pd.DataFrame()
    cohort = delisting_cohort.copy() if delisting_cohort is not None else pd.DataFrame()
    if base.empty and cohort.empty:
        return pd.DataFrame(columns=FORWARD_RETURN_SS_COLUMNS)
    keys = ["security_id", "as_of_date", "horizon_days"]
    # Empty-frame guard (deviation from the brief's verbatim snippet, documented in the report):
    # the outer merge needs its join keys present on BOTH sides, but a bare pd.DataFrame() (the
    # no-surviving-bar and no-delist cases the verbatim tests exercise) carries no columns, so
    # reindex an empty side to its canonical columns. Non-empty inputs are untouched, so the
    # stitch / id-hash / sort semantics stay byte-identical to the brief.
    if base.empty:
        base = pd.DataFrame(columns=_FORWARD_RETURN_BASE_COLUMNS)
    if cohort.empty:
        cohort = pd.DataFrame(columns=_DELISTING_COHORT_COLUMNS)
    merged = base.merge(cohort, on=keys, how="outer", suffixes=("", "_c"))
    merged["is_delisted_in_horizon"] = merged["terminal_return"].notna()
    merged["is_stitched"] = merged["is_delisted_in_horizon"]
    merged["forward_return"] = [
        _stitch(r, t) for r, t in zip(merged.get("raw_forward_return"), merged.get("terminal_return"))
    ]
    merged["available_at"] = (
        pd.concat([pd.to_datetime(merged["available_at"]),
                   pd.to_datetime(merged["terminal_available_at"])], axis=1).max(axis=1)
    )
    merged = merged[merged["forward_return"].notna()].copy()
    merged["source"] = source
    merged["run_id"] = run_id
    merged["is_latest_revision"] = True
    merged = merged.sort_values(keys, kind="mergesort").reset_index(drop=True)
    merged["forward_return_id"] = [
        _forward_return_id(source, s, a, h)
        for s, a, h in zip(merged["security_id"], merged["as_of_date"], merged["horizon_days"])
    ]
    return merged[FORWARD_RETURN_SS_COLUMNS]


def refresh_survivorship_safe_forward_returns(
    store: DuckDBStore,
    options: SurvivorshipSafeForwardReturnOptions | None = None,
) -> int:
    """Replace one source atomically using spillable SQL and one horizon at a time.

    A survivor requires a positive finite price on the exact h-th observed calendar
    session. A known terminal in (formation, endpoint] instead uses the latest
    positive price STRICTLY BEFORE delisting, compounded with the terminal return.
    Event-day and later bars never enter that leg; formations on/after a known
    terminal are excluded. A formation-only price produces a zero partial return.
    Invalid selected terminal values retain that event boundary but suppress labels
    spanning it; the input-derived quality gate fails instead of reviving old data.

    ``adjusted_close`` is the production basis; ``close`` explicitly opts into raw
    price compatibility. Missing adjusted prices are never inferred from raw close
    or a NULL split factor. Source adjustment quality remains a measured limitation.
    All input availability clocks are maximized. An optional observation cutoff is
    applied BEFORE revision selection, including terminal rows no longer marked
    latest. Calendar dates beyond that cutoff are excluded too. The cutoff is an
    observation vintage, not the formation date: these are realized outcome labels.

    Only scalar counts cross into Python. Temporary tables and the output inserts
    remain inside DuckDB's memory/spill controls; no all-bar/expanded pandas frames.
    """
    options = options or SurvivorshipSafeForwardReturnOptions()
    if options.price_basis not in {"adjusted_close", "close"}:
        raise ValueError("price_basis must be adjusted_close or close")
    cutoff = options.observation_cutoff
    if cutoff is not None and cutoff.tzinfo is not None:
        cutoff = cutoff.astimezone(dt.UTC).replace(tzinfo=None)
    store.initialize()
    con = store.con
    temporary_tables = ("_ss_bars", "_ss_calendar", "_ss_terminals", "_ss_terminal_prices")
    try:
        with store.transaction():
            con.execute(
                f"""
                CREATE TEMP TABLE _ss_bars AS
                WITH eligible AS (
                    SELECT *, coalesce(available_at,
                        CAST(trade_date AS TIMESTAMP) + INTERVAL '22 hours') AS price_available_at
                    FROM equity_daily_bars
                    WHERE (?::TIMESTAMP IS NULL OR coalesce(available_at,
                        CAST(trade_date AS TIMESTAMP) + INTERVAL '22 hours') <= ?)
                ), chosen AS (
                    SELECT security_id, symbol, trade_date,
                           {options.price_basis} AS price, price_available_at,
                           row_number() OVER (
                               PARTITION BY security_id, trade_date
                               ORDER BY price_available_at DESC, source ASC,
                                        vendor_security_id ASC NULLS LAST, symbol ASC,
                                        adjusted_close DESC NULLS LAST, close DESC NULLS LAST,
                                        source_loaded_at DESC
                           ) AS pick
                    FROM eligible
                )
                SELECT security_id, symbol, trade_date, price, price_available_at
                FROM chosen WHERE pick = 1 AND price > 0 AND isfinite(price)
                """,
                [cutoff, cutoff],
            )
            con.execute(
                """
                CREATE TEMP TABLE _ss_calendar AS
                SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_number
                FROM (
                    SELECT DISTINCT trade_date FROM trading_calendar
                    WHERE calendar_id = ? AND source = ? AND is_open
                      AND (?::TIMESTAMP IS NULL OR trade_date <= CAST(? AS DATE))
                )
                """,
                [_TRADING_CALENDAR_ID, _TRADING_CALENDAR_SOURCE, cutoff, cutoff],
            )
            con.execute(
                """
                CREATE TEMP TABLE _ss_terminals AS
                WITH revisions AS (
                    SELECT *, row_number() OVER (
                        PARTITION BY security_id, delist_date
                        ORDER BY available_at DESC, source_loaded_at DESC,
                                 terminal_return_id DESC
                    ) AS revision
                    FROM delisting_terminal_returns
                    WHERE (?::TIMESTAMP IS NULL OR available_at <= ?)
                )
                SELECT security_id, delist_date, terminal_return, terminal_return_source,
                       return_observation_id, available_at AS terminal_available_at,
                       isfinite(terminal_return) AND terminal_return >= -1 AS terminal_valid
                FROM revisions WHERE revision = 1
                QUALIFY row_number() OVER (
                    PARTITION BY security_id ORDER BY delist_date, terminal_return_id
                ) = 1
                """,
                [cutoff, cutoff],
            )
            con.execute(
                """
                CREATE TEMP TABLE _ss_terminal_prices AS
                SELECT t.*, b.trade_date AS last_price_date, b.price AS last_price,
                       b.price_available_at AS last_price_available_at
                FROM _ss_terminals t
                ASOF LEFT JOIN _ss_bars b
                  ON t.security_id = b.security_id AND t.delist_date > b.trade_date
                """
            )
            con.execute(
                "DELETE FROM forward_returns_survivorship_safe WHERE source = ?", [options.source]
            )
            for horizon in IC_HORIZONS:
                con.execute(
                    f"""
                    INSERT INTO forward_returns_survivorship_safe
                        ({', '.join(FORWARD_RETURN_SS_COLUMNS)})
                    WITH legs AS (
                        SELECT f.security_id, f.symbol, f.trade_date AS as_of_date,
                               ending.trade_date AS forward_end_date,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.last_price / f.price - 1
                                    ELSE e.price / f.price - 1 END AS raw_forward_return,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.terminal_return END AS terminal_return,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.delist_date END AS delist_date,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.terminal_return_source END AS terminal_return_source,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.return_observation_id END AS return_observation_id,
                               greatest(f.price_available_at,
                                   CASE WHEN t.delist_date <= ending.trade_date
                                        THEN greatest(t.last_price_available_at,
                                                      t.terminal_available_at)
                                        ELSE e.price_available_at END) AS available_at
                        FROM _ss_bars f
                        JOIN _ss_calendar anchor ON anchor.trade_date = f.trade_date
                        JOIN _ss_calendar ending
                          ON ending.session_number = anchor.session_number + ?
                        LEFT JOIN _ss_bars e
                          ON e.security_id = f.security_id AND e.trade_date = ending.trade_date
                        LEFT JOIN _ss_terminal_prices t ON t.security_id = f.security_id
                        WHERE (t.delist_date IS NULL OR f.trade_date < t.delist_date)
                          -- Retain invalid selected evidence as an event boundary: never
                          -- resurrect an older return or use a post-terminal survivor leg.
                          AND (t.delist_date IS NULL OR t.delist_date > ending.trade_date
                               OR coalesce(t.terminal_valid, false))
                          AND ((t.delist_date <= ending.trade_date
                                AND t.last_price_date >= f.trade_date) OR e.price IS NOT NULL)
                    ), returns AS (
                        SELECT *, CASE WHEN terminal_return IS NOT NULL
                            THEN (1 + raw_forward_return) * (1 + terminal_return) - 1
                            ELSE raw_forward_return END AS forward_return
                        FROM legs
                    )
                    SELECT sha256(concat_ws('|', ?, security_id,
                                           CAST(as_of_date AS VARCHAR), CAST(? AS VARCHAR))),
                           ?, security_id, symbol, as_of_date, ?, forward_end_date,
                           raw_forward_return, terminal_return, forward_return,
                           terminal_return IS NOT NULL, terminal_return IS NOT NULL,
                           delist_date, terminal_return_source, return_observation_id,
                           true, available_at, ?
                    FROM returns WHERE isfinite(forward_return)
                    """,
                    [horizon, options.source, horizon, options.source, horizon, options.run_id],
                )
            count = con.execute(
                "SELECT count(*) FROM forward_returns_survivorship_safe WHERE source = ?",
                [options.source],
            ).fetchone()[0]
    finally:
        for table in reversed(temporary_tables):
            con.execute(f"DROP TABLE IF EXISTS {table}")
    return int(count)


def survivorship_forward_return_diagnostics(
    store: DuckDBStore,
    options: SurvivorshipSafeForwardReturnOptions | None = None,
) -> dict[str, Any]:
    """Scalar operational counts; missing adjustment/terminal inputs remain explicit.

    Source-row counts are not distinct security/date coverage or economic quality
    certification. The input-derived DQC separately identifies missing stitches.
    """
    options = options or SurvivorshipSafeForwardReturnOptions()
    cutoff = options.observation_cutoff
    if cutoff is not None and cutoff.tzinfo is not None:
        cutoff = cutoff.astimezone(dt.UTC).replace(tzinfo=None)
    prices = store.con.execute(
        """
        SELECT count(*) FILTER (WHERE close > 0 AND isfinite(close)),
               count(*) FILTER (WHERE close > 0 AND isfinite(close)
                   AND (adjusted_close IS NULL OR adjusted_close <= 0
                        OR NOT isfinite(adjusted_close)))
        FROM equity_daily_bars
        WHERE (?::TIMESTAMP IS NULL OR coalesce(available_at,
            CAST(trade_date AS TIMESTAMP) + INTERVAL '22 hours') <= ?)
        """, [cutoff, cutoff],
    ).fetchone()
    output = store.con.execute(
        """
        SELECT count(*), count(*) FILTER (WHERE is_stitched)
        FROM forward_returns_survivorship_safe WHERE source = ?
        """, [options.source],
    ).fetchone()
    uncovered = store.con.execute(
        """
        SELECT count(*) FROM delisting_events e
        WHERE e.security_id IS NOT NULL
          AND (?::TIMESTAMP IS NULL OR e.available_at <= ?)
          AND NOT EXISTS (
              SELECT 1 FROM delisting_terminal_returns t
              WHERE t.security_id = e.security_id AND t.delist_date = e.delist_date
                AND (?::TIMESTAMP IS NULL OR t.available_at <= ?)
          )
        """, [cutoff, cutoff, cutoff, cutoff],
    ).fetchone()[0]
    return {
        "source": options.source,
        "price_basis": options.price_basis,
        "calendar_basis": "bar-observed sessions; not an official exchange calendar",
        "adjustment_quality": "source supplied; economic and historical vintage quality unverified",
        "observation_cutoff": None if cutoff is None else cutoff.isoformat(),
        "positive_raw_price_source_rows": int(prices[0]),
        "missing_adjusted_price_source_rows": int(prices[1]),
        "rows": int(output[0]),
        "stitched_rows": int(output[1]),
        "uncovered_event_rows": int(uncovered),
    }
