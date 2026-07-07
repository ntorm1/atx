from __future__ import annotations

import datetime as dt
import math
import re
import statistics
import uuid
from collections import defaultdict
from dataclasses import dataclass, replace
from typing import Iterable

from .connection import DuckDBStore
from .quality import (
    GateResult,
    QualityResult,
    Severity,
    _coerce_severity,
    run_warehouse_quality_checks,
    summarize_quality_gate,
)
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


def _latest_recorded_check(
    store: DuckDBStore,
    dataset_id: str,
    check_name: str,
    *,
    recorded_since: dt.datetime | dt.date | str | None = None,
):
    """Return the most recently recorded ``data_quality_checks`` row for one check, or None.

    ``recorded_since`` bounds the read to the current run (PF4-S2 S2-1 review hardening,
    clause G): when supplied, only a row recorded at/after that step-start timestamp
    counts, so a stale prior-run outcome can neither satisfy nor falsely halt the current
    gate. Absence of a fresh row returns ``None`` -> the caller treats the check as
    not-evaluated-this-cycle (fail-open), never fail-closed. ``check_id`` (the
    ``data_quality_checks`` primary key) is the deterministic tiebreaker so two rows with
    an identical ``checked_at`` resolve to the same pick on every run.
    """

    since = _parse_timestamp(recorded_since) if recorded_since is not None else None
    params: list[object] = [dataset_id, check_name]
    since_clause = ""
    if since is not None:
        since_clause = "AND checked_at >= ?"
        params.append(since)
    row = store.con.execute(
        f"""
        SELECT status, observed_value, threshold_value, table_name
        FROM data_quality_checks
        WHERE dataset_id = ? AND check_name = ?
        {since_clause}
        ORDER BY checked_at DESC, check_id DESC
        LIMIT 1
        """,
        params,
    ).fetchone()
    return row


# PF4-S1's leakage/coverage DQC (signal_eval.LEAKAGE_DQC_CHECK_NAME /
# COVERAGE_DQC_CHECK_NAME) are facts this assembler CONSUMES from data_quality_checks,
# never re-derives. They cannot be included in evaluate_panel_gate's own narrow live
# sweep: db.quality._runner.run_warehouse_quality_checks wires signal_eval_dqc_results
# in behind a *dataset_id* match, and once dataset_ids includes "factor_panel" that
# match fires unconditionally (independent of any check_names narrowing), which would
# silently re-run S1's evaluator on every gate check and overwrite whatever it most
# recently recorded with a same-cycle recompute -- exactly the re-implementation the
# PF4-S2 mandate forbids. Excluding them here keeps the live sweep scoped to the
# schema/export-contract checks it legitimately owns, while leakage/coverage always
# fold in through the "latest recorded" path below, matching the PIT (G) contract:
# "the gate reads only already-materialized panel rows + recorded outcomes."
_PANEL_GATE_RECORDED_ONLY_CHECK_NAMES: frozenset[str] = frozenset(
    {"factor_leakage_tplus0", "factor_coverage_asof_universe"}
)


def evaluate_panel_gate(
    store: DuckDBStore,
    dataset_id: str,
    *,
    record: bool = True,
    recorded_since: dt.datetime | dt.date | str | None = None,
) -> "GateResult":
    """Assemble the factor-panel halt decision from the ``panel_gate_config`` critical set.

    Reuses pf2-S10's ``run_warehouse_quality_checks`` (for the live schema/export-contract
    check(s) this assembler owns) and ``summarize_quality_gate`` (the unchanged
    critical->halt / error->partial / warning->pass decision) -- this never re-implements
    either. Each configured check's severity is re-tagged to the ``panel_gate_config``
    value so a check can be demoted (e.g. critical -> warning) without a code deploy.
    PF4-S1's leakage/coverage checks (``_PANEL_GATE_RECORDED_ONLY_CHECK_NAMES``) are never
    part of the live sweep; they -- and any other configured check the live sweep did not
    evaluate this cycle -- fall back to their latest recorded ``data_quality_checks``
    outcome, so the gate consumes historically-recorded facts rather than re-computing
    anything PF4-S1's evaluators already own.

    ``recorded_since`` (the orchestrator passes the step's start timestamp) bounds that
    recorded-row fallback to the current run: a stale prior-run leakage/coverage outcome
    is ignored (treated as not-evaluated-this-cycle -> fail-open pass), never consumed as
    if it described this build. It is deliberately fail-open, not fail-closed: the sprint
    risk section warns against false-halt flakiness, so absence of a fresh recorded row
    does not halt.
    """

    config_rows = store.con.execute(
        """
        SELECT check_name, severity, enabled
        FROM panel_gate_config
        WHERE dataset_id = ?
        ORDER BY check_name
        """,
        [dataset_id],
    ).fetchall()
    live_check_names = tuple(
        str(check_name)
        for check_name, _severity, enabled in config_rows
        if bool(enabled) and str(check_name) not in _PANEL_GATE_RECORDED_ONLY_CHECK_NAMES
    )
    live = (
        run_warehouse_quality_checks(store, record=record, check_names=live_check_names)
        if live_check_names
        else []
    )
    live_by_name = {result.check_name: idx for idx, result in enumerate(live)}
    results = list(live)
    for check_name, severity, enabled in config_rows:
        check_name = str(check_name)
        gate_severity = _coerce_severity(severity, "critical")
        if not bool(enabled):
            continue
        if check_name in live_by_name:
            idx = live_by_name[check_name]
            results[idx] = replace(results[idx], severity=gate_severity)
            continue
        recorded = _latest_recorded_check(
            store, dataset_id, check_name, recorded_since=recorded_since
        )
        if recorded is None:
            continue  # not yet evaluated this cycle -> treated as pass for the gate
        status, observed, threshold, table_name = recorded
        results.append(
            QualityResult(
                dataset_id=dataset_id,
                table_name=str(table_name or "data_quality_checks"),
                check_name=check_name,
                status=str(status),
                observed_value=None if observed is None else float(observed),
                threshold_value=None if threshold is None else float(threshold),
                details={"source": "panel_gate_config", "panel_gate": True},
                severity=gate_severity,
            )
        )
    return summarize_quality_gate(dataset_id, results)
