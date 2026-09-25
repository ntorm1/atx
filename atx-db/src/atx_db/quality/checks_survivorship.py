"""PF4-S4 S4-3: survivorship-bias DQC (critical, gate-ready) + DLSTCD-recon gate.

Two production checks over the survivorship-safe forward-return surface:

* ``survivorship_forward_return_drops_delisted_names`` -- **critical, gate-ready**. Anti-joins
  known terminal returns against their OWN formation bars and the input calendar/horizons.
  Missing stitches fail even when the entire output is empty; output rows never define the
  expected grid. Missing adjusted inputs also remain visible as missing stitches. Threshold ``0.0`` / comparator ``le`` -> RED (halt) when > 0, GREEN at 0.
* ``delisting_code_reconciliation_unresolved`` -- **error**. Counts
  ``reconciliation_status = 'unmapped'`` rows only. A legitimate vendor/proxy ``'mismatch'`` is
  signal (surfaced in ``v_delisting_return_coverage``), never a failure.

Both checks are registered as data in ``quality_check_registry`` by migration 0188 and evaluated
inside ``run_warehouse_quality_checks`` via :func:`survivorship_dqc_results` (mirroring the
``signal_eval_dqc_results`` lazy-hook precedent). This module is deliberately a **leaf** of the
``db.quality`` package -- it imports only ``._types``, ``..connection`` and the publisher's
dependency-free ``effective_terminals_sql`` (one halt-gap dating rule for writer and gate), and
never ``._checks`` or ``._runner`` -- so it introduces no import cycle inside the package (enforced by
``test_decomposed_package_import_graphs_are_acyclic``). The small evaluation/registry helpers below
are therefore intentionally self-contained rather than reusing the private ``_checks`` runner
internals.
"""

from __future__ import annotations

import datetime as dt
from typing import Mapping

from .._forward_return_publication import effective_terminals_sql
from ..connection import DuckDBStore
from ._types import Comparator, QualityRegistryEntry, QualityResult, Severity, SqlQualityCheck


SURVIVORSHIP_FORWARD_RETURN_CHECK_NAME = "survivorship_forward_return_drops_delisted_names"
DELISTING_CODE_RECONCILIATION_CHECK_NAME = "delisting_code_reconciliation_unresolved"
SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME = "delisting_events_without_terminal_return"

SURVIVORSHIP_DATASET_ID = "forward_returns_survivorship_safe"
RECONCILIATION_DATASET_ID = "delisting_code_reconciliation"

# Keep this leaf independent of signal_eval (which imports quality). The focused
# contract test pins these horizons to signal_eval.IC_HORIZONS.
_SURVIVORSHIP_HORIZONS = (1, 5, 10, 21, 63)
_DEFAULT_FORWARD_SOURCE = "atx_forward_returns_survivorship_safe_v1"


def _survivorship_sql(
    *, source: str = _DEFAULT_FORWARD_SOURCE,
    price_basis: str = "adjusted_close",
    observation_cutoff: dt.datetime | None = None,
) -> str:
    """Expected stitches come from inputs, even when the output is wholly empty.

    A positive raw formation bar with a missing/invalid adjusted price still demands
    a stitch in production mode. The writer cannot compute it, so the gate reports
    the missing adjusted input rather than passing over an empty eligible set.
    Selected invalid terminal inputs fail once per event, even without a formation
    window. Their revisions/events are selected before numeric validity is tested.
    """
    if price_basis not in {"adjusted_close", "close"}:
        raise ValueError("price_basis must be adjusted_close or close")
    cutoff = observation_cutoff
    if cutoff is not None and cutoff.tzinfo is not None:
        cutoff = cutoff.astimezone(dt.UTC).replace(tzinfo=None)
    cutoff_sql = "NULL::TIMESTAMP" if cutoff is None else f"TIMESTAMP '{cutoff.isoformat()}'"
    source_sql = "'" + source.replace("'", "''") + "'"
    horizons = ", ".join(str(h) for h in _SURVIVORSHIP_HORIZONS)
    return f"""
    WITH revisions AS (
        SELECT *, row_number() OVER (
            PARTITION BY security_id, delist_date
            ORDER BY available_at DESC, source_loaded_at DESC, terminal_return_id DESC
        ) AS revision
        FROM delisting_terminal_returns
        WHERE ({cutoff_sql} IS NULL OR available_at <= {cutoff_sql})
    ), terminals AS (
        SELECT *, coalesce(isfinite(terminal_return) AND terminal_return >= -1, false)
            AS terminal_valid
        FROM revisions WHERE revision = 1
        QUALIFY row_number() OVER (
            PARTITION BY security_id ORDER BY delist_date, terminal_return_id
        ) = 1
    ), eligible_bars AS (
        SELECT *, coalesce(available_at,
            CAST(trade_date AS TIMESTAMP) + INTERVAL '22 hours') AS price_available_at
        FROM equity_daily_bars
        WHERE security_id IN (SELECT security_id FROM terminals)
          AND ({cutoff_sql} IS NULL OR coalesce(available_at,
              CAST(trade_date AS TIMESTAMP) + INTERVAL '22 hours') <= {cutoff_sql})
    ), bars AS (
        SELECT *, row_number() OVER (
            PARTITION BY security_id, trade_date
            ORDER BY price_available_at DESC, source ASC,
                     vendor_security_id ASC NULLS LAST, symbol ASC,
                     adjusted_close DESC NULLS LAST, close DESC NULLS LAST,
                     source_loaded_at DESC
        ) AS pick
        FROM eligible_bars
    ), cal AS (
        SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_number
        FROM (
            SELECT DISTINCT trade_date FROM trading_calendar
            WHERE calendar_id = 'XNYS' AND source = 'equity_daily_bars calendar' AND is_open
              AND ({cutoff_sql} IS NULL OR trade_date <= CAST({cutoff_sql} AS DATE))
        )
    ), printed AS (
        -- Last trade for halt-gap dating: any positive print (basis OR raw close), so a
        -- missing adjusted input dates the gap later than the writer can and stays RED.
        SELECT security_id, trade_date, price_available_at,
               CASE WHEN {price_basis} > 0 AND isfinite({price_basis})
                    THEN {price_basis} ELSE close END AS price
        FROM bars
        WHERE pick = 1 AND (({price_basis} > 0 AND isfinite({price_basis}))
                            OR (close > 0 AND isfinite(close)))
    ), effective AS ({effective_terminals_sql(terminals="terminals", bars="printed", calendar="cal")}
    ), horizons AS (SELECT unnest([{horizons}]) AS horizon_days), expected AS (
        SELECT b.security_id, b.trade_date AS as_of_date, h.horizon_days,
               ending.trade_date AS forward_end_date, t.effective_delist_date AS delist_date,
               t.return_observation_id, t.terminal_return_source,
               t.terminal_valid,
               greatest(b.price_available_at, t.available_at) AS minimum_available_at
        FROM bars b
        JOIN effective t ON t.security_id = b.security_id AND b.trade_date < t.effective_delist_date
        JOIN cal anchor ON anchor.trade_date = b.trade_date
        CROSS JOIN horizons h
        JOIN cal ending ON ending.session_number = anchor.session_number + h.horizon_days
        WHERE b.pick = 1 AND t.effective_delist_date <= ending.trade_date
          AND ((b.{price_basis} > 0 AND isfinite(b.{price_basis}))
               OR (b.close > 0 AND isfinite(b.close)))
    )
    SELECT (
        (SELECT count(*) FROM terminals WHERE NOT terminal_valid)
        + (SELECT count(*) FROM expected e WHERE e.terminal_valid AND NOT EXISTS (
            SELECT 1 FROM forward_returns_survivorship_safe f
            WHERE f.source = {source_sql} AND f.security_id = e.security_id
              AND f.as_of_date = e.as_of_date AND f.horizon_days = e.horizon_days
              AND f.forward_end_date = e.forward_end_date AND f.delist_date = e.delist_date
              AND f.return_observation_id IS NOT DISTINCT FROM e.return_observation_id
              AND f.terminal_return_source = e.terminal_return_source
              AND f.is_stitched AND f.is_latest_revision
              AND f.symbol IS NOT NULL AND isfinite(f.forward_return)
              AND f.available_at >= e.minimum_available_at
              AND ({cutoff_sql} IS NULL OR f.available_at <= {cutoff_sql})
        ))
    )::DOUBLE
    """


_SURVIVORSHIP_SQL = _survivorship_sql()

# Only 'unmapped' rows count. 'mismatch' is an expected, non-failing vendor/proxy disagreement.
_RECONCILIATION_SQL = """
SELECT count(*)::DOUBLE
FROM delisting_code_reconciliation
WHERE reconciliation_status = 'unmapped'
"""

# The critical drop check above anti-joins delisting_terminal_returns against the panel, so an
# EMPTY delisting_terminal_returns makes it pass on an empty set -- "no delisted name with a
# known terminal return was dropped" is not "the universe is complete" (audit 4.4 / 8 #7). This
# companion check measures the other side: every delisting_events row must have a terminal
# return. Zero delistings is still zero here, but the moment any delisting evidence exists with
# no terminal return the gate goes RED instead of silently green.
_TERMINAL_RETURN_COVERAGE_SQL = """
SELECT count(*)::DOUBLE
FROM delisting_events e
LEFT JOIN delisting_terminal_returns t
  ON t.security_id = e.security_id
 AND t.delist_date = e.delist_date
WHERE e.security_id IS NOT NULL
  AND t.terminal_return_id IS NULL
"""

_SEVERITIES = frozenset({"warning", "error", "critical"})
_COMPARATORS = frozenset({"eq", "le", "ge"})


def _survivorship_spec(
    *, source: str = _DEFAULT_FORWARD_SOURCE,
    price_basis: str = "adjusted_close",
    observation_cutoff: dt.datetime | None = None,
) -> SqlQualityCheck:
    return SqlQualityCheck(
        dataset_id=SURVIVORSHIP_DATASET_ID,
        table_name="forward_returns_survivorship_safe",
        check_name=SURVIVORSHIP_FORWARD_RETURN_CHECK_NAME,
        sql=_survivorship_sql(
            source=source, price_basis=price_basis, observation_cutoff=observation_cutoff,
        ),
        threshold=0.0,
        comparator="le",
        required_tables=(
            "delisting_terminal_returns",
            "forward_returns_survivorship_safe",
            "equity_daily_bars",
            "trading_calendar",
        ),
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


def _terminal_return_coverage_spec() -> SqlQualityCheck:
    return SqlQualityCheck(
        dataset_id="delisting_terminal_returns",
        table_name="delisting_terminal_returns",
        check_name=SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME,
        sql=_TERMINAL_RETURN_COVERAGE_SQL,
        threshold=0.0,
        comparator="le",
        required_tables=("delisting_events", "delisting_terminal_returns"),
        warn_if_missing=True,
        failure_status="failed",
        severity="error",
    )


def survivorship_check_specs(**_ignored: object) -> tuple[SqlQualityCheck, ...]:
    """The three S4-3/S4-4 survivorship-safety check specs.

    Accepts and ignores the ``daily_macro_stale_days``/``monthly_macro_stale_days``/
    ``valuation_stale_gap_days`` common kwargs so it is interchangeable with the other
    ``*_check_specs`` factories, even though none of these checks need them.
    """

    return (_survivorship_spec(), _reconciliation_spec(), _terminal_return_coverage_spec())


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
    store: DuckDBStore, *, checked_at: dt.datetime | None = None,
    source: str = _DEFAULT_FORWARD_SOURCE, price_basis: str = "adjusted_close",
    observation_cutoff: dt.datetime | None = None,
) -> QualityResult:
    """Run the registered critical survivorship-drop check and return its ``QualityResult``.

    RED (``status='failed'``, ``severity='critical'``) when a delisted name that should have been
    stitched into a formation window is absent/unstitched; GREEN (``'passed'``) at zero drops.
    """

    return _run_single_check(
        store, _survivorship_spec(
            source=source, price_basis=price_basis, observation_cutoff=observation_cutoff,
        ), checked_at=checked_at,
    )


def delisting_code_reconciliation_check(
    store: DuckDBStore, *, checked_at: dt.datetime | None = None
) -> QualityResult:
    """Run the registered DLSTCD-reconciliation gate (``severity='error'``).

    Fires only on ``reconciliation_status='unmapped'`` rows; expected ``'mismatch'`` disagreements
    never fail.
    """

    return _run_single_check(store, _reconciliation_spec(), checked_at=checked_at)


def delisting_terminal_return_coverage_check(
    store: DuckDBStore, *, checked_at: dt.datetime | None = None
) -> QualityResult:
    """Count delisting events carrying no terminal return (the anti-vacuity gate).

    Companion to :func:`survivorship_forward_return_check`: that critical check can pass on an
    empty anti-join when ``delisting_terminal_returns`` is empty; this ``error``-severity check
    makes that specific gap loud instead of silently green.
    """

    return _run_single_check(store, _terminal_return_coverage_spec(), checked_at=checked_at)


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
