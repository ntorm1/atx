"""PF4-S4 S4-3: survivorship-bias DQC (critical, gate-ready) + DLSTCD-recon gate.

Two production checks over the survivorship-safe forward-return surface:

* ``survivorship_forward_return_drops_delisted_names`` -- **critical, gate-ready**. Anti-joins
  every observed/policy delisting terminal return against the panel's formation-date grid: a
  delisted name whose ``delist_date`` falls inside a formation window that some surviving name
  *was* evaluated over, but which carries no stitched ``forward_returns_survivorship_safe`` row,
  is a survivorship drop. Threshold ``0.0`` / comparator ``le`` -> RED (halt) when > 0, GREEN at 0.
* ``delisting_code_reconciliation_unresolved`` -- **error**. Counts
  ``reconciliation_status = 'unmapped'`` rows only. A legitimate vendor/proxy ``'mismatch'`` is
  signal (surfaced in ``v_delisting_return_coverage``), never a failure.

Both checks are registered as data in ``quality_check_registry`` by migration 0188 and evaluated
inside ``run_warehouse_quality_checks`` via :func:`survivorship_dqc_results` (mirroring the
``signal_eval_dqc_results`` lazy-hook precedent). This module is deliberately a **leaf** of the
``db.quality`` package -- it imports only ``._types`` and ``..connection`` and never ``._checks``
or ``._runner`` -- so it introduces no import cycle inside the package (enforced by
``test_decomposed_package_import_graphs_are_acyclic``). The small evaluation/registry helpers below
are therefore intentionally self-contained rather than reusing the private ``_checks`` runner
internals.
"""

from __future__ import annotations

import datetime as dt
from typing import Mapping

from ..connection import DuckDBStore
from ._types import Comparator, QualityRegistryEntry, QualityResult, Severity, SqlQualityCheck


SURVIVORSHIP_FORWARD_RETURN_CHECK_NAME = "survivorship_forward_return_drops_delisted_names"
DELISTING_CODE_RECONCILIATION_CHECK_NAME = "delisting_code_reconciliation_unresolved"

SURVIVORSHIP_DATASET_ID = "forward_returns_survivorship_safe"
RECONCILIATION_DATASET_ID = "delisting_code_reconciliation"

# Anti-join (S4-3 brief). The formation-date grid is derived security-INDEPENDENTLY -- a fully
# dropped name has zero rows of its own in forward_returns_survivorship_safe, so the grid of
# windows that were actually evaluated must come from the surviving names' rows. For each observed
# /policy terminal return whose delist falls in such a window, the absence of a *stitched* row for
# that same (security_id, as_of_date, horizon_days) is a survivorship drop.
_SURVIVORSHIP_SQL = """
SELECT count(*)::DOUBLE
FROM delisting_terminal_returns t
JOIN (
    SELECT DISTINCT as_of_date, horizon_days, forward_end_date
    FROM forward_returns_survivorship_safe
) panel
  ON t.delist_date >  panel.as_of_date
 AND t.delist_date <= panel.forward_end_date
LEFT JOIN forward_returns_survivorship_safe f
  ON f.security_id  = t.security_id
 AND f.as_of_date   = panel.as_of_date
 AND f.horizon_days = panel.horizon_days
 AND f.is_stitched
WHERE f.forward_return_id IS NULL   -- a delisted name in a formation window that was NOT stitched
"""

# Only 'unmapped' rows count. 'mismatch' is an expected, non-failing vendor/proxy disagreement.
_RECONCILIATION_SQL = """
SELECT count(*)::DOUBLE
FROM delisting_code_reconciliation
WHERE reconciliation_status = 'unmapped'
"""

_SEVERITIES = frozenset({"warning", "error", "critical"})
_COMPARATORS = frozenset({"eq", "le", "ge"})


def _survivorship_spec() -> SqlQualityCheck:
    return SqlQualityCheck(
        dataset_id=SURVIVORSHIP_DATASET_ID,
        table_name="forward_returns_survivorship_safe",
        check_name=SURVIVORSHIP_FORWARD_RETURN_CHECK_NAME,
        sql=_SURVIVORSHIP_SQL,
        threshold=0.0,
        comparator="le",
        required_tables=("delisting_terminal_returns", "forward_returns_survivorship_safe"),
        warn_if_missing=True,
        failure_status="failed",
        severity="critical",
    )


def _reconciliation_spec() -> SqlQualityCheck:
    return SqlQualityCheck(
        dataset_id=RECONCILIATION_DATASET_ID,
        table_name="delisting_code_reconciliation",
        check_name=DELISTING_CODE_RECONCILIATION_CHECK_NAME,
        sql=_RECONCILIATION_SQL,
        threshold=0.0,
        comparator="le",
        required_tables=("delisting_code_reconciliation",),
        warn_if_missing=True,
        failure_status="failed",
        severity="error",
    )


def survivorship_check_specs(**_ignored: object) -> tuple[SqlQualityCheck, ...]:
    """The two S4-3 survivorship-safety check specs.

    Accepts and ignores the ``daily_macro_stale_days``/``monthly_macro_stale_days``/
    ``valuation_stale_gap_days`` common kwargs so it is interchangeable with the other
    ``*_check_specs`` factories, even though neither check needs them.
    """

    return (_survivorship_spec(), _reconciliation_spec())


def _coerce_severity(value: object, fallback: Severity = "error") -> Severity:
    text = str(value or "").strip().lower()
    return text if text in _SEVERITIES else fallback  # type: ignore[return-value]


def _coerce_comparator(value: object, fallback: Comparator = "le") -> Comparator:
    text = str(value or "").strip().lower()
    return text if text in _COMPARATORS else fallback  # type: ignore[return-value]


def _passes(observed: float, threshold: float, comparator: Comparator) -> bool:
    if comparator == "le":
        return observed <= threshold
    if comparator == "ge":
        return observed >= threshold
    return observed == threshold


def _relation_exists(store: DuckDBStore, name: str) -> bool:
    row = store.con.execute(
        """
        SELECT count(*)
        FROM (
            SELECT table_name AS relation_name FROM duckdb_tables() WHERE schema_name = 'main'
            UNION ALL
            SELECT view_name AS relation_name FROM duckdb_views() WHERE schema_name = 'main'
        )
        WHERE relation_name = ?
        """,
        [name],
    ).fetchone()[0]
    return bool(row)


def _registry_entry(store: DuckDBStore, check_name: str) -> QualityRegistryEntry | None:
    if not _relation_exists(store, "quality_check_registry"):
        return None
    rows = store.con.execute(
        """
        SELECT check_name, dataset_id, table_name, severity, threshold_value, comparator, enabled
        FROM quality_check_registry
        WHERE check_name = ?
        """,
        [check_name],
    ).fetchall()
    if not rows:
        return None
    check_name, dataset_id, table_name, severity, threshold, comparator, enabled = rows[0]
    return QualityRegistryEntry(
        check_name=str(check_name),
        dataset_id=str(dataset_id),
        table_name=None if table_name is None else str(table_name),
        severity=_coerce_severity(severity),
        threshold_value=None if threshold is None else float(threshold),
        comparator=None if comparator is None else _coerce_comparator(comparator),
        enabled=bool(enabled),
    )


def _now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)


def _evaluate(
    store: DuckDBStore,
    spec: SqlQualityCheck,
    entry: QualityRegistryEntry | None,
    *,
    checked_at: dt.datetime,
) -> QualityResult:
    """Evaluate ``spec`` honoring any ``quality_check_registry`` override.

    Mirrors the registry-resolution semantics of the shared runner (severity / threshold /
    comparator come from the registry row when present) without importing its private helpers,
    keeping this module a leaf of ``db.quality``.
    """

    severity: Severity = entry.severity if entry is not None else (spec.severity or "error")
    threshold = (
        entry.threshold_value
        if entry is not None and entry.threshold_value is not None
        else spec.threshold
    )
    comparator: Comparator = (
        entry.comparator if entry is not None and entry.comparator is not None else spec.comparator
    )

    missing = [table for table in spec.required_tables if not _relation_exists(store, table)]
    if missing:
        return QualityResult(
            dataset_id=spec.dataset_id,
            table_name=spec.table_name,
            check_name=spec.check_name,
            status="skipped",
            observed_value=None,
            threshold_value=threshold,
            details={"missing_tables": missing, "checked_at": checked_at.isoformat()},
            severity=severity,
        )

    observed = store.con.execute(spec.sql).fetchone()[0]
    observed_value = None if observed is None else float(observed)
    passed = observed_value is not None and _passes(observed_value, threshold, comparator)
    return QualityResult(
        dataset_id=spec.dataset_id,
        table_name=spec.table_name,
        check_name=spec.check_name,
        status="passed" if passed else spec.failure_status,
        observed_value=observed_value,
        threshold_value=threshold,
        details={
            "comparator": comparator,
            "required_tables": spec.required_tables,
            "checked_at": checked_at.isoformat(),
        },
        severity=severity,
    )


def _run_single_check(
    store: DuckDBStore, spec: SqlQualityCheck, *, checked_at: dt.datetime | None
) -> QualityResult:
    checked_at = checked_at or _now()
    entry = _registry_entry(store, spec.check_name)
    if entry is not None and not entry.enabled:
        return QualityResult(
            dataset_id=spec.dataset_id,
            table_name=spec.table_name,
            check_name=spec.check_name,
            status="skipped",
            observed_value=None,
            threshold_value=spec.threshold,
            details={"reason": "check disabled in registry", "checked_at": checked_at.isoformat()},
            severity=entry.severity,
        )
    return _evaluate(store, spec, entry, checked_at=checked_at)


def survivorship_forward_return_check(
    store: DuckDBStore, *, checked_at: dt.datetime | None = None
) -> QualityResult:
    """Run the registered critical survivorship-drop check and return its ``QualityResult``.

    RED (``status='failed'``, ``severity='critical'``) when a delisted name that should have been
    stitched into a formation window is absent/unstitched; GREEN (``'passed'``) at zero drops.
    """

    return _run_single_check(store, _survivorship_spec(), checked_at=checked_at)


def delisting_code_reconciliation_check(
    store: DuckDBStore, *, checked_at: dt.datetime | None = None
) -> QualityResult:
    """Run the registered DLSTCD-reconciliation gate (``severity='error'``).

    Fires only on ``reconciliation_status='unmapped'`` rows; expected ``'mismatch'`` disagreements
    never fail.
    """

    return _run_single_check(store, _reconciliation_spec(), checked_at=checked_at)


def _check_requested(
    spec: SqlQualityCheck,
    *,
    requested_checks: set[str] | None,
    requested_datasets: set[str] | None,
) -> bool:
    if requested_checks is None and requested_datasets is None:
        return True
    if requested_checks is not None and spec.check_name in requested_checks:
        return True
    if requested_datasets is not None and spec.dataset_id in requested_datasets:
        return True
    return False


def survivorship_dqc_results(
    store: DuckDBStore,
    *,
    registry: Mapping[str, QualityRegistryEntry],
    requested_checks: set[str] | None,
    requested_datasets: set[str] | None,
    checked_at: dt.datetime,
) -> list[QualityResult]:
    """The S4-3 gate-ready checks, emitted for the shared ``run_warehouse_quality_checks`` sweep.

    Mirrors ``signal_eval_dqc_results``: each spec is gated by the registry ``enabled`` flag and the
    same requested-checks / requested-datasets narrowing, and required-table existence is probed
    lazily so a warehouse predating these surfaces emits ``status='skipped'`` rather than halting.
    Recording is the runner's responsibility -- this function has no side effects.
    """

    results: list[QualityResult] = []
    for spec in survivorship_check_specs():
        entry = registry.get(spec.check_name)
        if entry is not None and not entry.enabled:
            continue
        if not _check_requested(
            spec, requested_checks=requested_checks, requested_datasets=requested_datasets
        ):
            continue
        results.append(_evaluate(store, spec, entry, checked_at=checked_at))
    return results
