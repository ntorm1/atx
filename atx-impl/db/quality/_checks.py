from __future__ import annotations

import datetime as dt
import json
from dataclasses import replace
from typing import Mapping, Sequence

from ..connection import DuckDBStore
from ..schema_contract import (
    ColumnSpec,
    PIT_COLUMN_NAMES,
    SIGN_VALUES,
    _fetch_catalogued_tables,
    _fetch_live_tables,
    build_contract_manifest,
)
from ..warehouse import quality_check
from ._check_common import (
    DEFAULT_EXPORT_OBJECTS,
    DEFAULT_VALUATION_STALE_GAP_DAYS,
    INTERNAL_ONLY_EXPORT_FORBIDDEN_COLUMN,
    _export_scan_internal_cusip_sql,
)
from .checks_analytics import analytic_check_specs as _analytic_check_specs
from .checks_estimates import estimate_check_specs as _estimate_check_specs
from .checks_features_catalog import feature_catalog_check_specs as _feature_catalog_check_specs
from .checks_fundamentals import fundamental_check_specs as _fundamental_check_specs
from .checks_market_reference import (
    market_reference_check_specs as _market_reference_check_specs,
)
from .checks_ownership import ownership_check_specs as _ownership_check_specs
from ._types import (
    Comparator,
    FailureStatus,
    PASS_STATUSES,
    QualityRegistryEntry,
    QualityResult,
    ReferentialQualityCheck,
    SEVERITY_RANK,
    Severity,
    SqlQualityCheck,
    _SemanticContractColumn,
)


# PF2-S1 S1-1: check_name constants for the two schema-contract gates below, so tests
# and (later) PF2-S10's orchestrator gating never have to hand-copy the literal string.
CATALOG_COMPLETENESS_CHECK_NAME = "catalog_completeness"
PIT_COLUMN_PRESENCE_CHECK_NAME = "pit_column_presence"
SEMANTIC_CONTRACT_CHECK_NAME = "semantic_contract"
PIT_EXEMPTION_TABLE_NAME = "pit_exemption"
SCHEMA_CONTRACT_DATASET_ID = "schema_contract"
SCHEMA_CONTRACT_TABLE_NAME = "schema_contract"

_NUMERIC_DATA_TYPES = frozenset(
    {
        "TINYINT",
        "SMALLINT",
        "INTEGER",
        "BIGINT",
        "HUGEINT",
        "UTINYINT",
        "USMALLINT",
        "UINTEGER",
        "UBIGINT",
        "UHUGEINT",
        "FLOAT",
        "REAL",
        "DOUBLE",
        "DECIMAL",
        "NUMERIC",
    }
)
_SCHEMA_CONTRACT_SEMANTIC_COLUMNS = frozenset(
    {
        "table_name",
        "column_name",
        "data_type",
        "nullable",
        "is_natural_key",
        "is_pit_column",
        "declared_in",
        "unit",
        "sign",
        "scale",
        "natural_key",
    }
)

# PF-S7 S7-1: the statement-layer parent that fundamental_ratios rows resolve
# against. TODAY this is fundamental_points (raw SEC companyfacts). Once
# PF-S3/PF-S8 promote fundamental_statement_points to the ratio engine's
# primary statement-layer input, repoint this constant (or add a second
# ReferentialQualityCheck alongside the one that uses it in
# _referential_check_specs) -- a one-line upgrade.
PARENT_TABLE_FOR_FUNDAMENTAL_RATIOS = "fundamental_points"


def _main_objects(store: DuckDBStore) -> set[str]:
    return {
        row[0]
        for row in store.con.execute(
            """
            SELECT table_name AS object_name
            FROM duckdb_tables()
            WHERE schema_name = 'main'
              AND coalesce(internal, false) = false
            UNION
            SELECT view_name AS object_name
            FROM duckdb_views()
            WHERE schema_name = 'main'
              AND coalesce(internal, false) = false
            """
        ).fetchall()
    }


def _table_exists(
    store: DuckDBStore, table_name: str, *, objects: set[str] | None = None
) -> bool:
    if objects is not None:
        return table_name in objects
    return table_name in _main_objects(store)


def _quote_identifier(identifier: str) -> str:
    return '"' + identifier.replace('"', '""') + '"'


def _duckdb_type_family(data_type: str) -> str:
    return data_type.upper().split("(", 1)[0].strip()


def _is_numeric_data_type(data_type: str) -> bool:
    return _duckdb_type_family(data_type) in _NUMERIC_DATA_TYPES


def _parse_declared_bounds(scale: str | None) -> tuple[float | None, float | None] | None:
    if scale is None:
        return None
    text = scale.strip()
    if not text:
        return None
    if text.startswith("[") and text.endswith("]"):
        parts = text[1:-1].split(",", 1)
    elif ".." in text:
        parts = text.split("..", 1)
    else:
        return None
    if len(parts) != 2:
        return None
    lower_text, upper_text = (part.strip() for part in parts)
    try:
        lower = None if lower_text in {"", "-inf", "-infinity"} else float(lower_text)
        upper = None if upper_text in {"", "inf", "infinity"} else float(upper_text)
    except ValueError:
        return None
    return lower, upper


def _semantic_columns_from_manifest(
    manifest: Mapping[str, Sequence[ColumnSpec]],
) -> list[_SemanticContractColumn]:
    columns: list[_SemanticContractColumn] = []
    for table_name, specs in manifest.items():
        if not any(spec.is_pit_column for spec in specs):
            continue
        for spec in specs:
            columns.append(
                _SemanticContractColumn(
                    table_name=table_name,
                    column_name=spec.name,
                    data_type=spec.data_type,
                    sign=spec.sign,
                    unit=spec.unit,
                    scale=spec.scale,
                    natural_key=spec.natural_key,
                )
            )
    return sorted(columns, key=lambda column: (column.table_name, column.column_name))


def _schema_contract_semantic_columns(store: DuckDBStore) -> set[str]:
    if not _table_exists(store, SCHEMA_CONTRACT_TABLE_NAME):
        return set()
    return {
        row[0]
        for row in store.con.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE schema_name = 'main'
              AND table_name = ?
            """,
            [SCHEMA_CONTRACT_TABLE_NAME],
        ).fetchall()
    }


def _semantic_columns_from_schema_contract(
    store: DuckDBStore,
) -> tuple[list[_SemanticContractColumn], list[str]]:
    available_columns = _schema_contract_semantic_columns(store)
    missing_columns = sorted(_SCHEMA_CONTRACT_SEMANTIC_COLUMNS - available_columns)
    if missing_columns:
        return [], missing_columns

    rows = store.con.execute(
        """
        WITH fact_tables AS (
            SELECT table_name
            FROM schema_contract
            GROUP BY table_name
            HAVING max(CASE WHEN is_pit_column THEN 1 ELSE 0 END) > 0
        )
        SELECT
            sc.table_name,
            sc.column_name,
            sc.data_type,
            sc.sign,
            sc.unit,
            sc.scale,
            sc.natural_key
        FROM schema_contract AS sc
        JOIN fact_tables AS ft
          ON ft.table_name = sc.table_name
        ORDER BY sc.table_name, sc.column_name
        """
    ).fetchall()
    columns = [
        _SemanticContractColumn(
            table_name=str(table_name),
            column_name=str(column_name),
            data_type=str(data_type),
            sign=None if sign is None else str(sign),
            unit=None if unit is None else str(unit),
            scale=None if scale is None else str(scale),
            natural_key=None if natural_key is None else bool(natural_key),
        )
        for table_name, column_name, data_type, sign, unit, scale, natural_key in rows
    ]
    return columns, []


def _passes(observed_value: float, threshold: float, comparator: Comparator) -> bool:
    if comparator == "eq":
        return observed_value == threshold
    if comparator == "le":
        return observed_value <= threshold
    if comparator == "ge":
        return observed_value >= threshold
    raise ValueError(f"Unknown comparator {comparator!r}")


def severity_for_failure_status(failure_status: str) -> Severity:
    return "warning" if failure_status == "warning" else "error"


def _coerce_severity(value: object, fallback: Severity) -> Severity:
    text = str(value or "").strip().lower()
    if text in SEVERITY_RANK:
        return text  # type: ignore[return-value]
    return fallback


def _coerce_comparator(value: object, fallback: Comparator) -> Comparator:
    text = str(value or "").strip().lower()
    if text in {"eq", "le", "ge"}:
        return text  # type: ignore[return-value]
    return fallback


def _spec_default_severity(spec: SqlQualityCheck) -> Severity:
    return spec.severity or severity_for_failure_status(spec.failure_status)


def _quality_registry(store: DuckDBStore) -> dict[str, QualityRegistryEntry]:
    if not _table_exists(store, "quality_check_registry"):
        return {}
    rows = store.con.execute(
        """
        SELECT check_name, dataset_id, table_name, severity, threshold_value, comparator, enabled
        FROM quality_check_registry
        """
    ).fetchall()
    entries: dict[str, QualityRegistryEntry] = {}
    for check_name, dataset_id, table_name, severity, threshold, comparator, enabled in rows:
        entries[str(check_name)] = QualityRegistryEntry(
            check_name=str(check_name),
            dataset_id=str(dataset_id),
            table_name=None if table_name is None else str(table_name),
            severity=_coerce_severity(severity, "error"),
            threshold_value=None if threshold is None else float(threshold),
            comparator=None if comparator is None else _coerce_comparator(comparator, "eq"),
            enabled=bool(enabled),
        )
    return entries


def _resolve_spec(
    spec: SqlQualityCheck,
    registry: Mapping[str, QualityRegistryEntry],
) -> SqlQualityCheck | None:
    entry = registry.get(spec.check_name)
    if entry is not None and not entry.enabled:
        return None
    severity = entry.severity if entry is not None else _spec_default_severity(spec)
    threshold = (
        entry.threshold_value
        if entry is not None and entry.threshold_value is not None
        else spec.threshold
    )
    comparator = (
        entry.comparator
        if entry is not None and entry.comparator is not None
        else spec.comparator
    )
    return replace(spec, threshold=threshold, comparator=comparator, severity=severity)


def _apply_registry_to_result(
    result: QualityResult,
    registry: Mapping[str, QualityRegistryEntry],
) -> QualityResult | None:
    entry = registry.get(result.check_name)
    if entry is not None and not entry.enabled:
        return None
    severity = entry.severity if entry is not None else result.severity
    threshold = (
        entry.threshold_value
        if entry is not None and entry.threshold_value is not None
        else result.threshold_value
    )
    comparator = (
        entry.comparator
        if entry is not None and entry.comparator is not None
        else "eq"
    )
    status = result.status
    if result.observed_value is not None and threshold is not None:
        passed = _passes(float(result.observed_value), float(threshold), comparator)
        if passed:
            status = "passed"
        elif result.status in PASS_STATUSES:
            status = "failed"
    return replace(
        result,
        status=status,
        threshold_value=threshold,
        severity=severity,
        details={
            **result.details,
            "registry_comparator": comparator,
            "registry_threshold_value": threshold,
        },
    )


def _registry_allows_check(
    check_name: str,
    registry: Mapping[str, QualityRegistryEntry],
) -> bool:
    entry = registry.get(check_name)
    return entry is None or entry.enabled


def _record_quality_result(store: DuckDBStore, result: QualityResult) -> None:
    quality_check(
        store,
        dataset_id=result.dataset_id,
        table_name=result.table_name,
        check_name=result.check_name,
        status=result.status,
        severity=result.severity,
        observed_value=result.observed_value,
        threshold_value=result.threshold_value,
        details=result.details,
    )


def _referential_check_specs() -> tuple[ReferentialQualityCheck, ...]:
    """Orphan checks over the fundamentals DAG the ratio engine consumes.

    (a) Every ``fundamental_ratios`` row's security/period must resolve to
        backing raw fundamental facts -- see
        ``PARENT_TABLE_FOR_FUNDAMENTAL_RATIOS`` for the parameterized parent
        (``fundamental_points`` today).
    (b) Every fact's ``item_id`` (``fundamental_points``,
        ``fundamental_statement_points``) must resolve in the canonical item
        dimension (``fundamental_item``, PF-S1). ``item_id`` is nullable
        (many facts are not yet mapped to a governed item) -- a NULL
        ``item_id`` is skipped, not counted as an orphan.
    """
    return (
        ReferentialQualityCheck(
            dataset_id="fundamental_ratios",
            check_name="fundamental_ratios_without_fundamental_points",
            child_table="fundamental_ratios",
            parent_table=PARENT_TABLE_FOR_FUNDAMENTAL_RATIOS,
            child_keys=("security_id", "period_end"),
            parent_keys=("security_id", "period_end"),
        ),
        ReferentialQualityCheck(
            dataset_id="sec_company_facts",
            check_name="fundamental_points_item_without_fundamental_item",
            child_table="fundamental_points",
            child_key="item_id",
            parent_table="fundamental_item",
            parent_key="item_id",
        ),
        ReferentialQualityCheck(
            dataset_id="fundamental_statement_points",
            check_name="fundamental_statement_points_item_without_fundamental_item",
            child_table="fundamental_statement_points",
            child_key="item_id",
            parent_table="fundamental_item",
            parent_key="item_id",
        ),
    )


def _check_specs(
    *,
    daily_macro_stale_days: int,
    monthly_macro_stale_days: int,
    valuation_stale_gap_days: int = DEFAULT_VALUATION_STALE_GAP_DAYS,
) -> tuple[SqlQualityCheck, ...]:
    valuation_stale_gap_days = int(valuation_stale_gap_days)
    common_kwargs = {
        "daily_macro_stale_days": daily_macro_stale_days,
        "monthly_macro_stale_days": monthly_macro_stale_days,
        "valuation_stale_gap_days": valuation_stale_gap_days,
    }
    single_table_checks = (
        _market_reference_check_specs(**common_kwargs)
        + _fundamental_check_specs(**common_kwargs)
        + _ownership_check_specs(**common_kwargs)
        + _feature_catalog_check_specs(**common_kwargs)
        + _estimate_check_specs(**common_kwargs)
        + _analytic_check_specs(**common_kwargs)
    )
    referential_checks = tuple(spec.compile() for spec in _referential_check_specs())
    return single_table_checks + referential_checks


def catalog_completeness_check(
    store: DuckDBStore,
    *,
    checked_at: dt.datetime | None = None,
    objects: set[str] | None = None,
) -> QualityResult:
    """Every non-ephemeral live table must have a ``table_catalog`` row.

    PF2-S1 S1-1: `table_catalog` is hand-seeded (`schema.py::_seed_catalog` plus each
    migration's own insert) with nothing asserting ``duckdb_tables() ⊆ table_catalog``
    (sprint plan fact 2). This check closes that gap as a first-class, gate-ready
    (severity="critical") QualityResult -- PF2-S10 will later wire it into orchestrator
    gating (clause G, adopted incrementally); this sprint only authors the check.

    Reuses ``schema_contract.py``'s own live-table/catalogued-table readers verbatim
    (``_fetch_live_tables``/``_fetch_catalogued_tables``) so the ephemeral filter
    (``duckdb_tables()`` already excludes registered temp relations for free; the
    duckdb_%/sqlite_%/pragma_% internals are filtered explicitly) can never drift out
    of sync between the drift detector and this check.
    """
    checked_at = checked_at or dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
    con = store.con

    if not _table_exists(store, "table_catalog", objects=objects):
        return QualityResult(
            dataset_id=SCHEMA_CONTRACT_DATASET_ID,
            table_name="table_catalog",
            check_name=CATALOG_COMPLETENESS_CHECK_NAME,
            status="warning",
            observed_value=None,
            threshold_value=0.0,
            details={"missing_tables": ["table_catalog"], "checked_at": checked_at.isoformat()},
            severity="critical",
        )

    uncatalogued = sorted(_fetch_live_tables(con) - _fetch_catalogued_tables(con))
    observed_value = float(len(uncatalogued))
    passed = observed_value == 0.0
    return QualityResult(
        dataset_id=SCHEMA_CONTRACT_DATASET_ID,
        table_name="table_catalog",
        check_name=CATALOG_COMPLETENESS_CHECK_NAME,
        status="passed" if passed else "failed",
        observed_value=observed_value,
        threshold_value=0.0,
        details={"uncatalogued_tables": uncatalogued, "checked_at": checked_at.isoformat()},
        severity="critical",
    )


def _pit_exemption_registry(store: DuckDBStore) -> tuple[dict[str, set[str]], list[dict[str, str]]]:
    """Read valid PIT exemptions from ``pit_exemption``.

    Exemptions are intentionally narrow: a row can subtract only the declared
    canonical PIT columns for its own table, and only with a non-empty reason.
    Invalid rows are reported to the check caller and do not subtract anything.
    """
    if not _table_exists(store, PIT_EXEMPTION_TABLE_NAME):
        return {}, []

    rows = store.con.execute(
        """
        SELECT table_name, missing_columns, reason
        FROM pit_exemption
        """
    ).fetchall()
    canonical = set(PIT_COLUMN_NAMES)
    exemptions: dict[str, set[str]] = {}
    invalid: list[dict[str, str]] = []
    for table_name, missing_columns, reason in rows:
        table = str(table_name or "").strip()
        reason_text = str(reason or "").strip()
        raw_columns = str(missing_columns or "").strip()
        if not table:
            invalid.append({"table_name": table, "reason": "table_name is empty"})
            continue
        if not reason_text:
            invalid.append({"table_name": table, "reason": "reason is empty"})
            continue
        try:
            decoded = json.loads(raw_columns)
        except (TypeError, ValueError) as exc:
            invalid.append({"table_name": table, "reason": f"missing_columns is not JSON: {exc}"})
            continue
        if not isinstance(decoded, list):
            invalid.append({"table_name": table, "reason": "missing_columns must be a JSON array"})
            continue
        columns = {str(column).strip() for column in decoded if str(column).strip()}
        unknown = sorted(columns - canonical)
        if not columns:
            invalid.append({"table_name": table, "reason": "missing_columns is empty"})
            continue
        if unknown:
            invalid.append(
                {
                    "table_name": table,
                    "reason": f"unknown PIT columns: {', '.join(unknown)}",
                }
            )
            continue
        exemptions.setdefault(table, set()).update(columns)
    return exemptions, invalid


def pit_column_presence_check(
    store: DuckDBStore,
    *,
    manifest: Mapping[str, Sequence[ColumnSpec]] | None = None,
    checked_at: dt.datetime | None = None,
) -> QualityResult:
    """Every fact/derived table must carry all five canonical PIT columns.

    PF2-S1 S1-1: clause (A) requires every fact/derived row to carry ``as_of_date,
    available_at, source_loaded_at, run_id, is_latest_revision`` (sprint plan fact 5),
    but nothing asserted it -- a fact table shipped without ``available_at`` would pass
    every existing check and silently break as-of readers.

    The fact/non-fact partition comes from the S1-2 manifest
    (``schema_contract.build_contract_manifest``), NOT a hardcoded table list: a table
    counts as fact/derived iff at least one of its ``ColumnSpec``s has
    ``is_pit_column=True`` (equivalently, it carries >=1 strong bitemporal marker --
    see ``schema_contract._STRONG_TEMPORAL_MARKERS``). ``manifest`` defaults to a fresh
    ``build_contract_manifest(store.con)`` call; tests may inject a narrower manifest
    the same way ``detect_schema_drift``'s ``contract`` parameter does.
    """
    checked_at = checked_at or dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
    resolved_manifest: Mapping[str, Sequence[ColumnSpec]] = (
        manifest if manifest is not None else build_contract_manifest(store.con)
    )
    exemptions, invalid_exemptions = _pit_exemption_registry(store)

    offenders: dict[str, list[str]] = {}
    applied_exemptions: dict[str, list[str]] = {}
    for table_name, specs in resolved_manifest.items():
        is_fact = any(spec.is_pit_column for spec in specs)
        if not is_fact:
            continue
        spec_names = {spec.name for spec in specs}
        missing_set = set(PIT_COLUMN_NAMES) - spec_names
        exempted = sorted(missing_set & exemptions.get(table_name, set()))
        if exempted:
            applied_exemptions[table_name] = exempted
        missing = sorted(missing_set - set(exempted))
        if missing:
            offenders[table_name] = missing

    observed_value = float(len(offenders) + len(invalid_exemptions))
    passed = observed_value == 0.0
    return QualityResult(
        dataset_id=SCHEMA_CONTRACT_DATASET_ID,
        table_name="schema_contract",
        check_name=PIT_COLUMN_PRESENCE_CHECK_NAME,
        status="passed" if passed else "failed",
        observed_value=observed_value,
        threshold_value=0.0,
        details={
            "tables_missing_pit_columns": offenders,
            "exempted_pit_columns": applied_exemptions,
            "invalid_pit_exemptions": invalid_exemptions,
            "checked_at": checked_at.isoformat(),
        },
        severity="critical",
    )
def _semantic_violation_predicate(
    column: _SemanticContractColumn, quoted_column: str
) -> tuple[str | None, str | None, str | None]:
    sign = column.sign
    data_type = _duckdb_type_family(column.data_type)
    unit = (column.unit or "").strip().lower()
    scale = (column.scale or "").strip().lower()
    value = f"TRY_CAST({quoted_column} AS DOUBLE)"
    non_finite = f"NOT isfinite({value})"

    if sign not in SIGN_VALUES:
        return None, None, f"unknown semantic sign {sign!r}"
    if sign == "signed":
        return None, "signed", None
    if sign == "bounded":
        bounds = _parse_declared_bounds(column.scale)
        if bounds is None and (unit == "flag" or scale == "boolean"):
            if data_type == "BOOLEAN":
                return None, "bounded(boolean physical type)", None
            if not _is_numeric_data_type(column.data_type):
                return None, None, "bounded boolean/flag domain is not numeric or BOOLEAN"
            return f"{non_finite} OR {value} NOT IN (0.0, 1.0)", "bounded(boolean 0/1)", None
        if bounds is None:
            return None, "bounded(no declared numeric bounds)", None
        if not _is_numeric_data_type(column.data_type):
            return None, None, "bounded domain declares numeric bounds on a non-numeric column"
        lower, upper = bounds
        predicates: list[str] = []
        predicates.append(non_finite)
        if lower is not None:
            predicates.append(f"{value} < {float(lower)!r}")
        if upper is not None:
            predicates.append(f"{value} > {float(upper)!r}")
        domain = (
            f"bounded({lower}, {upper})"
            if lower is not None or upper is not None
            else "bounded(finite numeric)"
        )
        return " OR ".join(predicates), domain, None

    if not _is_numeric_data_type(column.data_type):
        return None, None, f"{sign} domain is not numeric"
    if sign == "non_negative":
        return f"{non_finite} OR {value} < 0.0", "non_negative", None
    if sign == "non_positive":
        return f"{non_finite} OR {value} > 0.0", "non_positive", None
    if sign == "unit_interval":
        return f"{non_finite} OR {value} < 0.0 OR {value} > 1.0", "unit_interval[0,1]", None
    return None, None, f"unsupported semantic sign {sign!r}"


def _incomplete_semantic_declaration_reason(column: _SemanticContractColumn) -> str | None:
    missing: list[str] = []
    if column.unit is None or not str(column.unit).strip():
        missing.append("unit")
    if column.sign is None or not str(column.sign).strip():
        missing.append("sign")
    if column.scale is None or not str(column.scale).strip():
        missing.append("scale")
    if column.natural_key is None:
        missing.append("natural_key")
    if not missing:
        return None
    return "missing semantic declaration fields: " + ", ".join(missing)


def semantic_contract_check(
    store: DuckDBStore,
    *,
    manifest: Mapping[str, Sequence[ColumnSpec]] | None = None,
    checked_at: dt.datetime | None = None,
) -> QualityResult:
    """Validate fact-column values against the S2-1 semantic tier.

    The semantic tier is read from the persisted ``schema_contract`` rows by
    default, or from an injected manifest in small offline fixtures. Domain
    rules are driven by each column's declared ``sign``/``unit``/``scale``:
    ``signed`` columns are deliberately unconstrained, while constrained signs
    fail on bad values.
    """
    checked_at = checked_at or dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
    source = "manifest" if manifest is not None else SCHEMA_CONTRACT_TABLE_NAME

    if manifest is not None:
        semantic_columns = _semantic_columns_from_manifest(manifest)
        missing_semantic_columns: list[str] = []
    else:
        if not _table_exists(store, SCHEMA_CONTRACT_TABLE_NAME):
            return QualityResult(
                dataset_id=SCHEMA_CONTRACT_DATASET_ID,
                table_name=SCHEMA_CONTRACT_TABLE_NAME,
                check_name=SEMANTIC_CONTRACT_CHECK_NAME,
                status="warning",
                observed_value=None,
                threshold_value=0.0,
                details={
                    "missing_tables": [SCHEMA_CONTRACT_TABLE_NAME],
                    "source": source,
                    "checked_at": checked_at.isoformat(),
                },
                severity="critical",
            )
        semantic_columns, missing_semantic_columns = _semantic_columns_from_schema_contract(store)

    objects = _main_objects(store)
    live_columns: dict[str, set[str]] = {}
    for table_name, column_name in store.con.execute(
        """
        SELECT table_name, column_name
        FROM duckdb_columns()
        WHERE schema_name = 'main'
        """
    ).fetchall():
        live_columns.setdefault(str(table_name), set()).add(str(column_name))

    violations: list[dict[str, object]] = []
    invalid_declarations: list[dict[str, str]] = []
    missing_targets: list[dict[str, str]] = []
    skipped_bounded_columns: list[dict[str, str]] = []
    checked_columns = 0
    signed_columns = 0

    for column in semantic_columns:
        incomplete_reason = _incomplete_semantic_declaration_reason(column)
        if incomplete_reason is not None:
            invalid_declarations.append(
                {
                    "table_name": column.table_name,
                    "column_name": column.column_name,
                    "reason": incomplete_reason,
                }
            )
            continue
        if column.sign not in SIGN_VALUES:
            invalid_declarations.append(
                {
                    "table_name": column.table_name,
                    "column_name": column.column_name,
                    "reason": f"unknown semantic sign {column.sign!r}",
                }
            )
            continue
        if column.table_name not in objects:
            missing_targets.append(
                {
                    "table_name": column.table_name,
                    "column_name": column.column_name,
                    "reason": "table is missing",
                }
            )
            continue
        if column.column_name not in live_columns.get(column.table_name, set()):
            missing_targets.append(
                {
                    "table_name": column.table_name,
                    "column_name": column.column_name,
                    "reason": "column is missing",
                }
            )
            continue
        if column.sign == "signed":
            signed_columns += 1
            continue

        quoted_column = _quote_identifier(column.column_name)
        predicate, domain, invalid_reason = _semantic_violation_predicate(column, quoted_column)
        if invalid_reason is not None:
            invalid_declarations.append(
                {
                    "table_name": column.table_name,
                    "column_name": column.column_name,
                    "reason": invalid_reason,
                }
            )
            continue
        if predicate is None:
            if domain and column.sign == "bounded":
                skipped_bounded_columns.append(
                    {
                        "table_name": column.table_name,
                        "column_name": column.column_name,
                        "domain": domain,
                    }
                )
            continue

        checked_columns += 1
        count = int(
            store.con.execute(
                f"""
                SELECT count(*)::BIGINT
                FROM {_quote_identifier(column.table_name)}
                WHERE {quoted_column} IS NOT NULL
                  AND ({predicate})
                """
            ).fetchone()[0]
        )
        if count:
            violations.append(
                {
                    "table_name": column.table_name,
                    "column_name": column.column_name,
                    "sign": column.sign,
                    "unit": column.unit,
                    "scale": column.scale,
                    "domain": domain,
                    "violation_count": count,
                }
            )

    observed_value = float(
        sum(int(row["violation_count"]) for row in violations)
        + len(invalid_declarations)
        + len(missing_targets)
        + len(missing_semantic_columns)
    )
    passed = observed_value == 0.0
    return QualityResult(
        dataset_id=SCHEMA_CONTRACT_DATASET_ID,
        table_name=SCHEMA_CONTRACT_TABLE_NAME,
        check_name=SEMANTIC_CONTRACT_CHECK_NAME,
        status="passed" if passed else "failed",
        observed_value=observed_value,
        threshold_value=0.0,
        details={
            "source": source,
            "violations": violations,
            "invalid_declarations": invalid_declarations,
            "missing_targets": missing_targets,
            "missing_schema_contract_semantic_columns": missing_semantic_columns,
            "skipped_bounded_columns": skipped_bounded_columns,
            "checked_columns": checked_columns,
            "signed_columns": signed_columns,
            "semantic_columns": len(semantic_columns),
            "checked_at": checked_at.isoformat(),
        },
        severity="critical",
    )
