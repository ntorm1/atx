from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from typing import Any, Iterable

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, now_utc_naive, quality_check


SOURCE_NAME = "atx-impl estimate security linker"

ESTIMATE_SECURITY_LINK_COLUMNS = [
    "est_security_link_id",
    "source",
    "provider",
    "vendor_security_id_type",
    "vendor_security_id",
    "symbol",
    "cusip",
    "target_security_id",
    "target_id_type",
    "target_id_value",
    "link_method",
    "link_status",
    "confidence",
    "decision_id",
    "candidate_id",
    "source_dataset_id",
    "evidence_table",
    "evidence_source",
    "valid_from",
    "valid_to",
    "as_of_date",
    "available_at",
    "first_observed_date",
    "last_observed_date",
    "first_observed_available_at",
    "observed_row_count",
    "observed_tables_json",
    "details_json",
    "run_id",
]

_METHOD_PRIORITY = {
    "identifier_resolution_decision_vendor_id": 400,
    "identifier_resolution_decision_cusip": 350,
    "security_identifier_history_vendor_id": 300,
    "security_identifier_history_cusip": 250,
}


@dataclass(frozen=True)
class EstimateSecurityLinkOptions:
    provider_names: tuple[str, ...] | None = None
    vendor_security_id_types: tuple[str, ...] | None = None
    min_confidence: float = 0.98
    apply_to_security_identifier_history: bool = True
    source: str = SOURCE_NAME
    run_id: str | None = None


def _clean_text(value: Any) -> str | None:
    if pd.isna(value):
        return None
    text = str(value).strip()
    return text or None


def _upper_text(value: Any) -> str | None:
    text = _clean_text(value)
    return text.upper() if text else None


def _as_date(value: Any) -> dt.date | None:
    if pd.isna(value):
        return None
    if isinstance(value, pd.Timestamp):
        return value.date()
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    parsed = pd.to_datetime(value, errors="coerce")
    if pd.isna(parsed):
        return None
    return parsed.date()


def _as_ts(value: Any) -> dt.datetime | None:
    if pd.isna(value):
        return None
    if isinstance(value, pd.Timestamp):
        return value.to_pydatetime().replace(tzinfo=None)
    if isinstance(value, dt.datetime):
        return value.replace(tzinfo=None)
    if isinstance(value, dt.date):
        return dt.datetime.combine(value, dt.time())
    parsed = pd.to_datetime(value, errors="coerce")
    if pd.isna(parsed):
        return None
    return parsed.to_pydatetime().replace(tzinfo=None)


def _max_date(*values: Any, default: dt.date | None = None) -> dt.date | None:
    parsed = [value for value in (_as_date(v) for v in values) if value is not None]
    if not parsed:
        return default
    return max(parsed)


def _max_ts(*values: Any, default: dt.datetime | None = None) -> dt.datetime | None:
    parsed = [value for value in (_as_ts(v) for v in values) if value is not None]
    if not parsed:
        return default
    return max(parsed)


def _hash_id(prefix: str, *parts: Any) -> str:
    payload = "|".join("" if pd.isna(part) else str(part).strip() for part in parts)
    digest = hashlib.sha256(payload.encode("utf-8")).hexdigest()[:16]
    return f"{prefix}-{digest}"


def _normalize_filter(values: Iterable[str] | None) -> set[str] | None:
    if values is None:
        return None
    normalized = {str(value).strip().upper() for value in values if str(value).strip()}
    return normalized or None


def _empty_links() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_SECURITY_LINK_COLUMNS)


def _estimate_key_frame(store: DuckDBStore, options: EstimateSecurityLinkOptions) -> pd.DataFrame:
    keys = store.con.execute(
        """
        WITH raw_keys AS (
            SELECT
                'est_detail' AS observed_table,
                provider,
                vendor_security_id_type,
                vendor_security_id,
                symbol,
                CAST(NULL AS VARCHAR) AS cusip,
                coalesce(as_of_date, estimate_date, announce_date, activation_date, period_end) AS observed_date,
                coalesce(available_at, source_loaded_at, now()) AS observed_available_at
            FROM est_detail
            WHERE vendor_security_id IS NOT NULL AND trim(vendor_security_id) <> ''

            UNION ALL

            SELECT
                'est_consensus' AS observed_table,
                provider,
                vendor_security_id_type,
                vendor_security_id,
                symbol,
                CAST(NULL AS VARCHAR) AS cusip,
                coalesce(as_of_date, consensus_date, period_end) AS observed_date,
                coalesce(available_at, source_loaded_at, now()) AS observed_available_at
            FROM est_consensus
            WHERE vendor_security_id IS NOT NULL AND trim(vendor_security_id) <> ''

            UNION ALL

            SELECT
                'est_recommendation' AS observed_table,
                provider,
                vendor_security_id_type,
                vendor_security_id,
                symbol,
                cusip,
                coalesce(as_of_date, rating_date, announce_date, activation_date) AS observed_date,
                coalesce(available_at, source_loaded_at, now()) AS observed_available_at
            FROM est_recommendation
            WHERE vendor_security_id IS NOT NULL AND trim(vendor_security_id) <> ''

            UNION ALL

            SELECT
                'est_recommendation_summary' AS observed_table,
                provider,
                vendor_security_id_type,
                vendor_security_id,
                symbol,
                cusip,
                coalesce(as_of_date, snapshot_date) AS observed_date,
                coalesce(available_at, source_loaded_at, now()) AS observed_available_at
            FROM est_recommendation_summary
            WHERE vendor_security_id IS NOT NULL AND trim(vendor_security_id) <> ''
        )
        SELECT *
        FROM raw_keys
        WHERE vendor_security_id_type IS NOT NULL
          AND trim(vendor_security_id_type) <> ''
          AND observed_date IS NOT NULL
          AND observed_available_at IS NOT NULL
        """
    ).df()
    if keys.empty:
        return keys

    keys["provider"] = keys["provider"].map(lambda value: _upper_text(value) or "INJECTED")
    keys["vendor_security_id_type"] = keys["vendor_security_id_type"].map(_upper_text)
    keys["vendor_security_id"] = keys["vendor_security_id"].map(_upper_text)
    keys["symbol"] = keys["symbol"].map(_upper_text)
    keys["cusip"] = keys["cusip"].map(_upper_text)
    keys["observed_date"] = keys["observed_date"].map(_as_date)
    keys["observed_available_at"] = keys["observed_available_at"].map(_as_ts)
    keys = keys[
        keys["vendor_security_id_type"].notna()
        & keys["vendor_security_id"].notna()
        & keys["observed_date"].notna()
        & keys["observed_available_at"].notna()
    ].copy()

    provider_filter = _normalize_filter(options.provider_names)
    type_filter = _normalize_filter(options.vendor_security_id_types)
    if provider_filter is not None:
        keys = keys[keys["provider"].isin(provider_filter)]
    if type_filter is not None:
        keys = keys[keys["vendor_security_id_type"].isin(type_filter)]
    if keys.empty:
        return keys

    rows: list[dict[str, Any]] = []
    group_cols = ["provider", "vendor_security_id_type", "vendor_security_id"]
    for key, group in keys.groupby(group_cols, dropna=False):
        provider, vendor_type, vendor_id = key
        rows.append(
            {
                "provider": provider,
                "vendor_security_id_type": vendor_type,
                "vendor_security_id": vendor_id,
                "symbol": next((value for value in group["symbol"] if value), None),
                "cusip": next((value for value in group["cusip"] if value), None),
                "first_observed_date": min(group["observed_date"]),
                "last_observed_date": max(group["observed_date"]),
                "first_observed_available_at": min(group["observed_available_at"]),
                "observed_row_count": int(len(group)),
                "observed_tables": sorted({str(value) for value in group["observed_table"]}),
            }
        )
    return pd.DataFrame(rows)


def _security_identifier_history(store: DuckDBStore) -> pd.DataFrame:
    frame = store.con.execute(
        """
        SELECT
            security_id,
            upper(id_type) AS id_type,
            upper(id_value) AS id_value,
            valid_from,
            valid_to,
            as_of_date,
            coalesce(available_at, source_loaded_at, now()) AS available_at,
            source
        FROM security_identifier_history
        WHERE id_type IS NOT NULL
          AND id_value IS NOT NULL
          AND trim(id_value) <> ''
        """
    ).df()
    if frame.empty:
        return frame
    frame["id_type"] = frame["id_type"].map(_upper_text)
    frame["id_value"] = frame["id_value"].map(_upper_text)
    return frame


def _accepted_identifier_decisions(
    store: DuckDBStore,
    options: EstimateSecurityLinkOptions,
) -> pd.DataFrame:
    frame = store.con.execute(
        """
        SELECT
            decision_id,
            candidate_id,
            source_dataset_id,
            source_table,
            upper(source_key_type) AS source_key_type,
            upper(source_key_value) AS source_key_value,
            target_security_id,
            target_id_type,
            target_id_value,
            match_method,
            confidence,
            effective_from,
            as_of_date,
            coalesce(available_at, decided_at, source_loaded_at, now()) AS available_at,
            decided_by
        FROM identifier_resolution_decisions
        WHERE decision_status = 'accepted'
          AND confidence >= ?
          AND source_key_type IS NOT NULL
          AND source_key_value IS NOT NULL
          AND trim(source_key_value) <> ''
        """,
        [options.min_confidence],
    ).df()
    if frame.empty:
        return frame
    frame["source_key_type"] = frame["source_key_type"].map(_upper_text)
    frame["source_key_value"] = frame["source_key_value"].map(_upper_text)
    return frame


def _candidate_from_history(
    key: pd.Series,
    evidence: pd.Series,
    *,
    method: str,
    source: str,
    run_id: str | None,
) -> dict[str, Any]:
    valid_from = _max_date(key["first_observed_date"], evidence["valid_from"], default=key["first_observed_date"])
    as_of_date = _max_date(key["first_observed_date"], evidence["as_of_date"], default=valid_from)
    available_at = _max_ts(
        key["first_observed_available_at"],
        evidence["available_at"],
        default=now_utc_naive(),
    )
    confidence = 0.995 if method == "security_identifier_history_vendor_id" else 0.99
    return _candidate_row(
        key,
        source=source,
        target_security_id=evidence["security_id"],
        target_id_type=evidence["id_type"],
        target_id_value=evidence["id_value"],
        method=method,
        confidence=confidence,
        evidence_table="security_identifier_history",
        evidence_source=evidence["source"],
        valid_from=valid_from,
        valid_to=_as_date(evidence["valid_to"]),
        as_of_date=as_of_date,
        available_at=available_at,
        decision_id=None,
        candidate_id=None,
        source_dataset_id=None,
        details={
            "history_id_type": evidence["id_type"],
            "history_id_value": evidence["id_value"],
            "history_valid_from": evidence["valid_from"],
            "history_valid_to": evidence["valid_to"],
            "history_as_of_date": evidence["as_of_date"],
        },
        run_id=run_id,
    )


def _candidate_from_decision(
    key: pd.Series,
    evidence: pd.Series,
    *,
    method: str,
    source: str,
    run_id: str | None,
) -> dict[str, Any]:
    valid_from = _max_date(
        key["first_observed_date"],
        evidence["effective_from"],
        default=key["first_observed_date"],
    )
    as_of_date = _max_date(key["first_observed_date"], evidence["as_of_date"], default=valid_from)
    available_at = _max_ts(
        key["first_observed_available_at"],
        evidence["available_at"],
        default=now_utc_naive(),
    )
    return _candidate_row(
        key,
        source=source,
        target_security_id=evidence["target_security_id"],
        target_id_type=evidence["target_id_type"],
        target_id_value=evidence["target_id_value"],
        method=method,
        confidence=float(evidence["confidence"]),
        evidence_table=evidence["source_table"],
        evidence_source=evidence["decided_by"],
        valid_from=valid_from,
        valid_to=None,
        as_of_date=as_of_date,
        available_at=available_at,
        decision_id=evidence["decision_id"],
        candidate_id=evidence["candidate_id"],
        source_dataset_id=evidence["source_dataset_id"],
        details={
            "decision_match_method": evidence["match_method"],
            "decision_source_key_type": evidence["source_key_type"],
            "decision_source_key_value": evidence["source_key_value"],
            "decision_effective_from": evidence["effective_from"],
        },
        run_id=run_id,
    )


def _candidate_row(
    key: pd.Series,
    *,
    source: str,
    target_security_id: Any,
    target_id_type: Any,
    target_id_value: Any,
    method: str,
    confidence: float,
    evidence_table: Any,
    evidence_source: Any,
    valid_from: dt.date | None,
    valid_to: dt.date | None,
    as_of_date: dt.date | None,
    available_at: dt.datetime | None,
    decision_id: Any,
    candidate_id: Any,
    source_dataset_id: Any,
    details: dict[str, Any],
    run_id: str | None,
) -> dict[str, Any]:
    provider = key["provider"]
    vendor_type = key["vendor_security_id_type"]
    vendor_id = key["vendor_security_id"]
    target_security_id = _clean_text(target_security_id)
    row = {
        "source": source,
        "provider": provider,
        "vendor_security_id_type": vendor_type,
        "vendor_security_id": vendor_id,
        "symbol": key.get("symbol"),
        "cusip": key.get("cusip"),
        "target_security_id": target_security_id,
        "target_id_type": _upper_text(target_id_type),
        "target_id_value": _upper_text(target_id_value),
        "link_method": method,
        "link_status": "accepted",
        "confidence": float(confidence),
        "decision_id": _clean_text(decision_id),
        "candidate_id": _clean_text(candidate_id),
        "source_dataset_id": _clean_text(source_dataset_id),
        "evidence_table": _clean_text(evidence_table),
        "evidence_source": _clean_text(evidence_source),
        "valid_from": valid_from,
        "valid_to": valid_to,
        "as_of_date": as_of_date,
        "available_at": available_at,
        "first_observed_date": key["first_observed_date"],
        "last_observed_date": key["last_observed_date"],
        "first_observed_available_at": key["first_observed_available_at"],
        "observed_row_count": int(key["observed_row_count"]),
        "observed_tables_json": json_dumps(key["observed_tables"]),
        "details_json": json_dumps(details),
        "run_id": run_id,
    }
    row["est_security_link_id"] = _hash_id(
        "EST-SEC-LINK",
        source,
        provider,
        vendor_type,
        vendor_id,
        target_security_id,
        method,
        row["decision_id"],
        row["valid_from"],
        row["available_at"],
    )
    return row


def build_estimate_security_links(
    store: DuckDBStore,
    options: EstimateSecurityLinkOptions,
) -> pd.DataFrame:
    if options.min_confidence < 0 or options.min_confidence > 1:
        raise ValueError("min_confidence must be in [0, 1]")

    keys = _estimate_key_frame(store, options)
    if keys.empty:
        return _empty_links()

    history = _security_identifier_history(store)
    decisions = _accepted_identifier_decisions(store, options)

    rows: list[dict[str, Any]] = []
    for _, key in keys.iterrows():
        if not history.empty:
            vendor_matches = history[
                (history["id_type"] == key["vendor_security_id_type"])
                & (history["id_value"] == key["vendor_security_id"])
            ]
            for _, evidence in vendor_matches.iterrows():
                rows.append(
                    _candidate_from_history(
                        key,
                        evidence,
                        method="security_identifier_history_vendor_id",
                        source=options.source,
                        run_id=options.run_id,
                    )
                )
            if key.get("cusip"):
                cusip_matches = history[
                    (history["id_type"] == "CUSIP")
                    & (history["id_value"] == key["cusip"])
                ]
                for _, evidence in cusip_matches.iterrows():
                    rows.append(
                        _candidate_from_history(
                            key,
                            evidence,
                            method="security_identifier_history_cusip",
                            source=options.source,
                            run_id=options.run_id,
                        )
                    )

        if not decisions.empty:
            vendor_decisions = decisions[
                (decisions["source_key_type"] == key["vendor_security_id_type"])
                & (decisions["source_key_value"] == key["vendor_security_id"])
            ]
            for _, evidence in vendor_decisions.iterrows():
                rows.append(
                    _candidate_from_decision(
                        key,
                        evidence,
                        method="identifier_resolution_decision_vendor_id",
                        source=options.source,
                        run_id=options.run_id,
                    )
                )
            if key.get("cusip"):
                cusip_decisions = decisions[
                    (decisions["source_key_type"] == "CUSIP")
                    & (decisions["source_key_value"] == key["cusip"])
                ]
                for _, evidence in cusip_decisions.iterrows():
                    rows.append(
                        _candidate_from_decision(
                            key,
                            evidence,
                            method="identifier_resolution_decision_cusip",
                            source=options.source,
                            run_id=options.run_id,
                        )
                    )

    if not rows:
        return _empty_links()

    candidates = pd.DataFrame(rows)
    candidates = candidates[candidates["confidence"] >= float(options.min_confidence)].copy()
    if candidates.empty:
        return _empty_links()

    selected: list[pd.DataFrame] = []
    group_cols = ["provider", "vendor_security_id_type", "vendor_security_id"]
    for _, group in candidates.groupby(group_cols, dropna=False):
        targets = {
            target for target in group["target_security_id"].dropna().astype(str)
            if target.strip()
        }
        if len(targets) > 1:
            conflicted = group.copy()
            conflicted["link_status"] = "conflict"
            selected.append(conflicted)
            continue
        ranked = group.copy()
        ranked["_method_priority"] = ranked["link_method"].map(_METHOD_PRIORITY).fillna(0)
        ranked = ranked.sort_values(
            by=[
                "confidence",
                "_method_priority",
                "as_of_date",
                "available_at",
                "est_security_link_id",
            ],
            ascending=[False, False, False, False, False],
        )
        best = ranked.head(1).drop(columns=["_method_priority"])
        selected.append(best)

    links = pd.concat(selected, ignore_index=True)
    return links[ESTIMATE_SECURITY_LINK_COLUMNS].drop_duplicates(subset=["est_security_link_id"])


def _replace_links(
    store: DuckDBStore,
    frame: pd.DataFrame,
    options: EstimateSecurityLinkOptions,
) -> int:
    with store.transaction():
        store.con.execute("DELETE FROM est_security_link WHERE source = ?", [options.source])
        if frame.empty:
            return 0
        insert_frame(store, frame, "est_security_link", "est_security_link_insert")
    return int(len(frame))


def _apply_identifier_history(
    store: DuckDBStore,
    frame: pd.DataFrame,
    options: EstimateSecurityLinkOptions,
) -> int:
    if frame.empty:
        return 0
    accepted = frame[
        (frame["link_status"] == "accepted")
        & frame["target_security_id"].notna()
        & frame["vendor_security_id_type"].notna()
        & frame["vendor_security_id"].notna()
    ].copy()
    if accepted.empty:
        return 0
    identifiers = pd.DataFrame(
        {
            "security_id": accepted["target_security_id"],
            "id_type": accepted["vendor_security_id_type"],
            "id_value": accepted["vendor_security_id"],
            "valid_from": accepted["valid_from"],
            "valid_to": accepted["valid_to"],
            "as_of_date": accepted["as_of_date"],
            "available_at": accepted["available_at"],
            "source": options.source,
            "run_id": options.run_id,
        }
    ).drop_duplicates(subset=["security_id", "id_type", "id_value", "valid_from"])
    store.con.register("estimate_security_link_identifiers", identifiers)
    try:
        store.con.execute(
            """
            DELETE FROM security_identifier_history
            USING estimate_security_link_identifiers src
            WHERE security_identifier_history.security_id = src.security_id
              AND security_identifier_history.id_type = src.id_type
              AND security_identifier_history.id_value = src.id_value
              AND security_identifier_history.valid_from = src.valid_from
              AND security_identifier_history.source = ?
            """,
            [options.source],
        )
    finally:
        store.con.unregister("estimate_security_link_identifiers")
    return insert_frame(
        store,
        identifiers,
        "security_identifier_history",
        "estimate_security_link_identifier_insert",
    )


class EstimateSecurityLinkDataset(Dataset):
    """PIT-safe vendor security identifier reconciliation for estimate rows."""

    dataset_id = "est_security_link"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: EstimateSecurityLinkOptions,
    ) -> DatasetLoadResult:
        links = build_estimate_security_links(store, options)
        rows_loaded = _replace_links(store, links, options)
        applied_identifier_rows = 0
        if options.apply_to_security_identifier_history and not links.empty:
            applied_identifier_rows = _apply_identifier_history(store, links, options)

        accepted_rows = int((links.get("link_status", pd.Series(dtype=str)) == "accepted").sum())
        conflict_rows = int((links.get("link_status", pd.Series(dtype=str)) == "conflict").sum())
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="est_security_link",
            check_name="rows_loaded",
            status="passed" if accepted_rows > 0 else "warning",
            observed_value=float(rows_loaded),
            threshold_value=1.0,
            details={
                "accepted_rows": accepted_rows,
                "conflict_rows": conflict_rows,
                "applied_identifier_rows": applied_identifier_rows,
                "min_confidence": options.min_confidence,
            },
        )

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=options.source,
            details={
                "accepted_rows": accepted_rows,
                "conflict_rows": conflict_rows,
                "applied_identifier_rows": applied_identifier_rows,
                "min_confidence": options.min_confidence,
            },
        )
