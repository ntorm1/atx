from __future__ import annotations

import datetime as dt
import math
import re
import statistics
import uuid
from collections import defaultdict
from dataclasses import dataclass
from typing import Iterable

from .connection import DuckDBStore
from .quality import QualityResult, Severity, _coerce_severity
from .warehouse import json_dumps, now_utc_naive, quality_check


@dataclass(frozen=True)
class DataQualityAnomaly:
    anomaly_id: str
    dataset_id: str
    check_name: str
    baseline_median: float
    baseline_mad: float
    observed_value: float
    z_score: float
    severity: Severity


def _slug(value: str) -> str:
    return re.sub(r"[^0-9a-zA-Z]+", "_", value).strip("_").lower() or "dataset"


def _table_exists(store: DuckDBStore, table_name: str) -> bool:
    row = store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = ?
        """,
        [table_name],
    ).fetchone()
    return bool(row and row[0])


def _parse_timestamp(value: object) -> dt.datetime | None:
    if value is None:
        return None
    if isinstance(value, dt.datetime):
        parsed = value
    elif isinstance(value, dt.date):
        parsed = dt.datetime.combine(value, dt.time.min)
    else:
        text = str(value).strip()
        if not text:
            return None
        try:
            parsed = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
        except ValueError:
            try:
                parsed = dt.datetime.combine(dt.date.fromisoformat(text[:10]), dt.time.min)
            except ValueError:
                return None
    if parsed.tzinfo is not None:
        parsed = parsed.astimezone(dt.timezone.utc).replace(tzinfo=None)
    return parsed


def _upsert_quality_registry(
    store: DuckDBStore,
    *,
    check_name: str,
    dataset_id: str,
    table_name: str,
    severity: Severity,
    threshold_value: float,
    comparator: str,
    failure_status: str,
    source: str,
) -> None:
    if not _table_exists(store, "quality_check_registry"):
        return
    store.con.execute(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name, dataset_id, table_name, severity, threshold_value,
            comparator, enabled, failure_status, source, updated_at
        )
        VALUES (?, ?, ?, ?, ?, ?, true, ?, ?, ?)
        """,
        [
            check_name,
            dataset_id,
            table_name,
            severity,
            threshold_value,
            comparator,
            failure_status,
            source,
            now_utc_naive(),
        ],
    )


def evaluate_freshness_slas(
    store: DuckDBStore,
    *,
    as_of: dt.datetime | dt.date | str | None = None,
) -> list[QualityResult]:
    """Evaluate dataset freshness SLAs against ``dataset_watermarks``.

    Fresh datasets produce no rows; stale or missing watermarks emit one
    severity-tagged ``data_quality_checks`` row per breached SLA.
    """

    if not _table_exists(store, "dataset_freshness_sla"):
        return []
    as_of_ts = _parse_timestamp(as_of) or now_utc_naive()
    rows = store.con.execute(
        """
        SELECT dataset_id, max_lag_days, severity
        FROM dataset_freshness_sla
        WHERE enabled
        ORDER BY dataset_id
        """
    ).fetchall()
    results: list[QualityResult] = []
    for dataset_id, max_lag_days, severity in rows:
        dataset_id = str(dataset_id)
        severity_value = _coerce_severity(severity, "warning")
        watermark_rows = store.con.execute(
            """
            SELECT watermark_name, watermark_value
            FROM dataset_watermarks
            WHERE dataset_id = ?
            ORDER BY watermark_name
            """,
            [dataset_id],
        ).fetchall()
        parsed = [
            (str(name), _parse_timestamp(value), str(value))
            for name, value in watermark_rows
        ]
        valid_marks = [(name, mark, raw) for name, mark, raw in parsed if mark is not None]
        latest = max((mark for _name, mark, _raw in valid_marks), default=None)
        max_lag = int(max_lag_days)
        lag_days = math.inf if latest is None else (as_of_ts - latest).total_seconds() / 86400.0
        if latest is not None and lag_days <= max_lag:
            continue
        check_name = f"freshness_sla_{_slug(dataset_id)}"
        status = "warning" if severity_value == "warning" else "failed"
        details = {
            "as_of": as_of_ts.isoformat(),
            "max_lag_days": max_lag,
            "latest_watermark": None if latest is None else latest.isoformat(),
            "watermarks": [
                {"watermark_name": name, "watermark_value": raw}
                for name, _mark, raw in parsed
            ],
        }
        observed = None if math.isinf(lag_days) else float(lag_days)
        result = QualityResult(
            dataset_id=dataset_id,
            table_name="dataset_watermarks",
            check_name=check_name,
            status=status,
            observed_value=observed,
            threshold_value=float(max_lag),
            details=details,
            severity=severity_value,
        )
        _upsert_quality_registry(
            store,
            check_name=check_name,
            dataset_id=dataset_id,
            table_name="dataset_watermarks",
            severity=severity_value,
            threshold_value=float(max_lag),
            comparator="le",
            failure_status="warning" if severity_value == "warning" else "failed",
            source="freshness_sla",
        )
        quality_check(
            store,
            dataset_id=dataset_id,
            table_name="dataset_watermarks",
            check_name=check_name,
            status=status,
            severity=severity_value,
            observed_value=observed,
            threshold_value=float(max_lag),
            details=details,
        )
        results.append(result)
    return results


def _median_mad(values: list[float]) -> tuple[float, float]:
    median = float(statistics.median(values))
    mad = float(statistics.median(abs(value - median) for value in values))
    return median, mad


def _robust_z(observed: float, median: float, mad: float) -> float:
    if mad == 0:
        if observed == median:
            return 0.0
        return math.copysign(math.inf, observed - median)
    return 0.6745 * (observed - median) / mad


def detect_rowcount_anomalies(
    store: DuckDBStore,
    *,
    window: int = 4,
    z_threshold: float = 3.5,
    severity: Severity = "warning",
    dataset_ids: Iterable[str] | None = None,
) -> list[DataQualityAnomaly]:
    """Detect count-style outliers from prior ``data_quality_checks`` history."""

    if window < 2:
        raise ValueError("window must be at least 2")
    if not _table_exists(store, "data_quality_anomaly"):
        return []
    requested = set(dataset_ids or ())
    rows = store.con.execute(
        """
        SELECT dataset_id, check_name, observed_value, checked_at
        FROM data_quality_checks
        WHERE observed_value IS NOT NULL
          AND check_name NOT LIKE 'rowcount_anomaly_%'
        ORDER BY dataset_id, check_name, checked_at
        """
    ).fetchall()
    grouped: dict[tuple[str, str], list[tuple[float, dt.datetime]]] = defaultdict(list)
    for dataset_id, check_name, observed, checked_at in rows:
        dataset_id = str(dataset_id)
        check_name = str(check_name)
        if requested and dataset_id not in requested:
            continue
        lowered = check_name.lower()
        if "count" not in lowered and "rows" not in lowered:
            continue
        grouped[(dataset_id, check_name)].append((float(observed), checked_at))

    anomalies: list[DataQualityAnomaly] = []
    checked_at = now_utc_naive()
    severity_value = _coerce_severity(severity, "warning")
    status = "warning" if severity_value == "warning" else "failed"
    for (dataset_id, source_check_name), series in sorted(grouped.items()):
        if len(series) <= window:
            continue
        prior = [value for value, _ts in series[-(window + 1) : -1]]
        observed = float(series[-1][0])
        median, mad = _median_mad(prior)
        z_score = _robust_z(observed, median, mad)
        if abs(z_score) < z_threshold:
            continue
        anomaly_check_name = f"rowcount_anomaly_{_slug(dataset_id)}_{_slug(source_check_name)}"
        anomaly_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"{dataset_id}:{source_check_name}:{checked_at.isoformat()}:{observed}"))
        details = {
            "source_check_name": source_check_name,
            "window": window,
            "z_threshold": z_threshold,
            "baseline_values": prior,
        }
        store.con.execute(
            """
            INSERT OR REPLACE INTO data_quality_anomaly (
                anomaly_id, dataset_id, check_name, baseline_median, baseline_mad,
                observed_value, z_score, is_anomaly, checked_at, severity,
                details_json, source_loaded_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, true, ?, ?, ?, ?)
            """,
            [
                anomaly_id,
                dataset_id,
                anomaly_check_name,
                median,
                mad,
                observed,
                z_score,
                checked_at,
                severity_value,
                json_dumps(details),
                checked_at,
            ],
        )
        _upsert_quality_registry(
            store,
            check_name=anomaly_check_name,
            dataset_id=dataset_id,
            table_name="data_quality_checks",
            severity=severity_value,
            threshold_value=float(z_threshold),
            comparator="le",
            failure_status="warning" if severity_value == "warning" else "failed",
            source="rowcount_anomaly",
        )
        quality_check(
            store,
            dataset_id=dataset_id,
            table_name="data_quality_checks",
            check_name=anomaly_check_name,
            status=status,
            severity=severity_value,
            observed_value=abs(float(z_score)) if math.isfinite(z_score) else float("inf"),
            threshold_value=float(z_threshold),
            details=details,
        )
        anomalies.append(
            DataQualityAnomaly(
                anomaly_id=anomaly_id,
                dataset_id=dataset_id,
                check_name=anomaly_check_name,
                baseline_median=median,
                baseline_mad=mad,
                observed_value=observed,
                z_score=z_score,
                severity=severity_value,
            )
        )
    return anomalies
