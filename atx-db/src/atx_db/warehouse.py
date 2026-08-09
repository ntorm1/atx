from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import uuid
from pathlib import Path
from typing import Any, Iterable

import pandas as pd

from .connection import DuckDBStore


OPEN_END_DATE = dt.date(9999, 12, 31)


def now_utc_naive() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)


def snake_case(value: str) -> str:
    value = re.sub(r"(?<!^)(?=[A-Z])", "_", value)
    value = re.sub(r"[^0-9a-zA-Z]+", "_", value)
    value = re.sub(r"_+", "_", value)
    return value.strip("_").lower()


def symbol_key(symbol: str | None) -> str:
    # NA-safe: pandas may pass pd.NA / NaN for blank ticker cells (the full
    # tbltickerhistory archive carries rows with no ticker in either column).
    # ``pd.NA or ""`` raises "boolean value of NA is ambiguous", so guard first.
    if symbol is None or symbol is pd.NA:
        return ""
    try:
        if pd.isna(symbol):
            return ""
    except (TypeError, ValueError):
        pass
    return str(symbol).strip().upper()


def cik_security_id(cik: str | int) -> str:
    return f"SEC-CIK-{int(cik):010d}"


def vendor_security_id(source: str, value: str | int) -> str:
    cleaned = str(value).strip()
    safe_source = re.sub(r"[^0-9A-Za-z]+", "-", source.strip().upper()).strip("-")
    return f"{safe_source}-{cleaned}"


def security_id_for_symbol(symbol: str) -> str:
    return f"US-TICKER-{symbol_key(symbol)}"


def security_id_for_cusip(cusip: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z]+", "", str(cusip).strip()).upper()
    return f"US-CUSIP-{cleaned}"


def json_dumps(payload: Any) -> str:
    return json.dumps(payload, sort_keys=True, default=str)


def file_sha256(path: Path, *, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def record_source_file(
    store: DuckDBStore,
    *,
    dataset_id: str,
    source_url: str,
    cache_path: Path | None = None,
    status: str = "available",
    metadata: dict[str, Any] | None = None,
    sha256: str | None = None,
    compute_hash: bool = False,
) -> str:
    source_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"{dataset_id}:{source_url}:{cache_path or ''}"))
    byte_count = cache_path.stat().st_size if cache_path and cache_path.exists() else None
    if sha256 is None and compute_hash and cache_path and cache_path.exists():
        sha256 = file_sha256(cache_path)

    store.con.execute("DELETE FROM raw_source_files WHERE source_id = ?", [source_id])
    store.con.execute(
        """
        INSERT INTO raw_source_files (
            source_id,
            dataset_id,
            source_url,
            cache_path,
            sha256,
            byte_count,
            fetched_at,
            status,
            metadata_json
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            source_id,
            dataset_id,
            source_url,
            str(cache_path) if cache_path else None,
            sha256,
            byte_count,
            now_utc_naive(),
            status,
            json_dumps(metadata or {}),
        ],
    )
    return source_id


def register_frame(store: DuckDBStore, name: str, frame: pd.DataFrame) -> None:
    store.con.register(name, frame)


def _table_columns(store: DuckDBStore, table: str) -> set[str]:
    rows = store.con.execute(
        """
        SELECT column_name
        FROM duckdb_columns()
        WHERE schema_name = 'main'
          AND table_name = ?
        """,
        [table],
    ).fetchall()
    return {str(row[0]) for row in rows}


def _insert_projection(frame: pd.DataFrame, target_columns: set[str]) -> tuple[str, str]:
    insert_columns = list(frame.columns)
    select_expressions = []

    for column in insert_columns:
        if column == "source_loaded_at" and column in target_columns:
            select_expressions.append("coalesce(CAST(source_loaded_at AS TIMESTAMP), now()) AS source_loaded_at")
        elif column == "is_latest_revision" and column in target_columns:
            select_expressions.append("coalesce(CAST(is_latest_revision AS BOOLEAN), true) AS is_latest_revision")
        else:
            select_expressions.append(column)

    if "source_loaded_at" in target_columns and "source_loaded_at" not in frame.columns:
        insert_columns.append("source_loaded_at")
        if "computed_at" in frame.columns:
            select_expressions.append("coalesce(CAST(computed_at AS TIMESTAMP), now()) AS source_loaded_at")
        else:
            select_expressions.append("now() AS source_loaded_at")

    if "is_latest_revision" in target_columns and "is_latest_revision" not in frame.columns:
        insert_columns.append("is_latest_revision")
        select_expressions.append("true AS is_latest_revision")

    return ", ".join(insert_columns), ", ".join(select_expressions)


def insert_frame(store: DuckDBStore, frame: pd.DataFrame, table: str, relation_name: str) -> int:
    if frame.empty:
        return 0
    columns, select_expressions = _insert_projection(frame, _table_columns(store, table))
    register_frame(store, relation_name, frame)
    try:
        store.con.execute(
            f"INSERT INTO {table} ({columns}) SELECT {select_expressions} FROM {relation_name}"
        )
    finally:
        store.con.unregister(relation_name)
    return int(len(frame))


def replace_by_relation(
    store: DuckDBStore,
    *,
    table: str,
    relation: str,
    key_columns: Iterable[str],
) -> None:
    predicates = " AND ".join(
        f"coalesce(cast(dst.{column} AS VARCHAR), '') = coalesce(cast(src.{column} AS VARCHAR), '')"
        for column in key_columns
    )
    store.con.execute(
        f"""
        DELETE FROM {table} AS dst
        USING {relation} AS src
        WHERE {predicates}
        """
    )


def quality_check(
    store: DuckDBStore,
    *,
    dataset_id: str,
    table_name: str,
    check_name: str,
    status: str,
    severity: str | None = None,
    observed_value: float | None = None,
    threshold_value: float | None = None,
    details: dict[str, Any] | None = None,
) -> None:
    resolved_severity = severity or ("warning" if status == "warning" else "error")
    store.con.execute(
        """
        INSERT INTO data_quality_checks (
            check_id,
            dataset_id,
            table_name,
            check_name,
            status,
            severity,
            observed_value,
            threshold_value,
            details_json,
            checked_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            str(uuid.uuid4()),
            dataset_id,
            table_name,
            check_name,
            status,
            resolved_severity,
            observed_value,
            threshold_value,
            json_dumps(details or {}),
            now_utc_naive(),
        ],
    )


def compact_date(value: str | dt.date | None) -> dt.date | None:
    if value is None or value == "":
        return None
    if isinstance(value, dt.date):
        return value
    return dt.date.fromisoformat(str(value))
