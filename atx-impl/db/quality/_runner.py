from __future__ import annotations

import datetime as dt
from typing import Iterable

from ._checks import (
    CATALOG_COMPLETENESS_CHECK_NAME,
    DEFAULT_VALUATION_STALE_GAP_DAYS,
    PIT_COLUMN_PRESENCE_CHECK_NAME,
    SCHEMA_CONTRACT_DATASET_ID,
    SEMANTIC_CONTRACT_CHECK_NAME,
    DuckDBStore,
    _apply_registry_to_result,
    _check_specs,
    _main_objects,
    _passes,
    _quality_registry,
    _record_quality_result,
    _registry_allows_check,
    _resolve_spec,
    catalog_completeness_check,
    pit_column_presence_check,
    semantic_contract_check,
)
from ._types import (
    GateDecision,
    GateResult,
    PASS_STATUSES,
    QualityResult,
    SEVERITY_RANK,
    Severity,
)


def summarize_quality_gate(dataset_id: str, results: Iterable[QualityResult]) -> GateResult:
    all_results = tuple(results)
    failed = tuple(result for result in all_results if result.status not in PASS_STATUSES)
    worst: Severity | None = None
    for result in failed:
        if worst is None or SEVERITY_RANK[result.severity] > SEVERITY_RANK[worst]:
            worst = result.severity
    if worst == "critical":
        decision: GateDecision = "halt"
    elif worst == "error":
        decision = "partial"
    else:
        decision = "pass"
    return GateResult(
        dataset_id=dataset_id,
        decision=decision,
        worst_severity=worst,
        result_count=len(all_results),
        failed_count=len(failed),
        failed_results=failed,
    )


def run_warehouse_quality_checks(
    store: DuckDBStore,
    *,
    daily_macro_stale_days: int = 10,
    monthly_macro_stale_days: int = 70,
    valuation_stale_gap_days: int = DEFAULT_VALUATION_STALE_GAP_DAYS,
    record: bool = True,
    check_names: Iterable[str] | None = None,
    dataset_ids: Iterable[str] | None = None,
) -> list[QualityResult]:
    """Run production-oriented SQL checks and optionally append check outcomes.

    ``check_names`` and ``dataset_ids`` are narrowing filters for targeted tests
    and local diagnosis. Leaving both unset preserves the full production sweep.
    """

    results: list[QualityResult] = []
    checked_at = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
    requested_checks = set(check_names) if check_names is not None else None
    requested_datasets = set(dataset_ids) if dataset_ids is not None else None
    objects = _main_objects(store)
    registry = _quality_registry(store)
    for spec in _check_specs(
        daily_macro_stale_days=daily_macro_stale_days,
        monthly_macro_stale_days=monthly_macro_stale_days,
        valuation_stale_gap_days=valuation_stale_gap_days,
    ):
        resolved_spec = _resolve_spec(spec, registry)
        if resolved_spec is None:
            continue
        spec = resolved_spec
        if (
            (requested_checks is not None or requested_datasets is not None)
            and not (
                (requested_checks is not None and spec.check_name in requested_checks)
                or (requested_datasets is not None and spec.dataset_id in requested_datasets)
            )
        ):
            continue

        missing_tables = [table for table in spec.required_tables if table not in objects]
        if missing_tables:
            status = "warning" if spec.warn_if_missing else "failed"
            result = QualityResult(
                dataset_id=spec.dataset_id,
                table_name=spec.table_name,
                check_name=spec.check_name,
                status=status,
                observed_value=None,
                threshold_value=spec.threshold,
                details={"missing_tables": missing_tables, "checked_at": checked_at.isoformat()},
                severity=spec.severity,
            )
        else:
            observed = store.con.execute(spec.sql).fetchone()[0]
            observed_value = None if observed is None else float(observed)
            passed = observed_value is not None and _passes(observed_value, spec.threshold, spec.comparator)
            details = {
                "comparator": spec.comparator,
                "required_tables": spec.required_tables,
                "checked_at": checked_at.isoformat(),
            }
            if not passed and spec.detail_sql:
                detail_cursor = store.con.execute(spec.detail_sql)
                detail_columns = [column[0] for column in detail_cursor.description or ()]
                details["rows"] = [
                    dict(zip(detail_columns, row, strict=True))
                    for row in detail_cursor.fetchall()
                ]
            result = QualityResult(
                dataset_id=spec.dataset_id,
                table_name=spec.table_name,
                check_name=spec.check_name,
                status="passed" if passed else spec.failure_status,
                observed_value=observed_value,
                threshold_value=spec.threshold,
                details=details,
                severity=spec.severity,
            )
        if record:
            _record_quality_result(store, result)
        results.append(result)

    # PF2-S1 S1-1: catalog-completeness + PIT-column-presence. Both read the S1-2
    # manifest / schema_contract.py's own introspection helpers rather than a static
    # SqlQualityCheck string (the fact/non-fact partition and the catalogued-table set
    # are runtime facts, not compile-time SQL), so they are computed directly and
    # folded into the same result list / record() path as every other check.
    schema_results: list[QualityResult] = []
    if (
        _registry_allows_check(CATALOG_COMPLETENESS_CHECK_NAME, registry)
        and (
            (requested_checks is None and requested_datasets is None)
            or (requested_checks is not None and CATALOG_COMPLETENESS_CHECK_NAME in requested_checks)
            or (requested_datasets is not None and SCHEMA_CONTRACT_DATASET_ID in requested_datasets)
        )
    ):
        schema_results.append(
            catalog_completeness_check(store, checked_at=checked_at, objects=objects)
        )
    if (
        _registry_allows_check(PIT_COLUMN_PRESENCE_CHECK_NAME, registry)
        and (
            (requested_checks is None and requested_datasets is None)
            or (requested_checks is not None and PIT_COLUMN_PRESENCE_CHECK_NAME in requested_checks)
            or (requested_datasets is not None and SCHEMA_CONTRACT_DATASET_ID in requested_datasets)
        )
    ):
        schema_results.append(pit_column_presence_check(store, checked_at=checked_at))
    if (
        _registry_allows_check(SEMANTIC_CONTRACT_CHECK_NAME, registry)
        and (
            (requested_checks is None and requested_datasets is None)
            or (requested_checks is not None and SEMANTIC_CONTRACT_CHECK_NAME in requested_checks)
            or (requested_datasets is not None and SCHEMA_CONTRACT_DATASET_ID in requested_datasets)
        )
    ):
        schema_results.append(semantic_contract_check(store, checked_at=checked_at))

    for result in schema_results:
        resolved_result = _apply_registry_to_result(result, registry)
        if resolved_result is None:
            continue
        result = resolved_result
        if record:
            _record_quality_result(store, result)
        results.append(result)

    return results


def evaluate_quality_gate(
    store: DuckDBStore,
    dataset_id: str,
    *,
    daily_macro_stale_days: int = 10,
    monthly_macro_stale_days: int = 70,
    valuation_stale_gap_days: int = DEFAULT_VALUATION_STALE_GAP_DAYS,
    record: bool = True,
    check_names: Iterable[str] | None = None,
    additional_results: Iterable[QualityResult] = (),
) -> GateResult:
    """Run and summarize the S10 quality gate for one dataset.

    ``additional_results`` lets dynamic observability checks (freshness/anomaly)
    use the same severity decision path after they have emitted their own
    ``data_quality_checks`` rows.
    """

    results = run_warehouse_quality_checks(
        store,
        daily_macro_stale_days=daily_macro_stale_days,
        monthly_macro_stale_days=monthly_macro_stale_days,
        valuation_stale_gap_days=valuation_stale_gap_days,
        record=record,
        check_names=check_names,
        dataset_ids=(dataset_id,),
    )
    results.extend(additional_results)
    return summarize_quality_gate(dataset_id, results)
