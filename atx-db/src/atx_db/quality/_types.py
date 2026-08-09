from __future__ import annotations

from dataclasses import dataclass
from typing import Literal


Comparator = Literal["eq", "le", "ge"]
FailureStatus = Literal["failed", "warning"]
Severity = Literal["critical", "error", "warning"]
GateDecision = Literal["pass", "partial", "halt"]
SEVERITY_RANK: dict[str, int] = {"warning": 1, "error": 2, "critical": 3}
PASS_STATUSES = frozenset({"passed", "skipped"})


@dataclass(frozen=True)
class SqlQualityCheck:
    dataset_id: str
    table_name: str
    check_name: str
    sql: str
    threshold: float
    comparator: Comparator = "eq"
    required_tables: tuple[str, ...] = ()
    warn_if_missing: bool = True
    failure_status: FailureStatus = "failed"
    detail_sql: str | None = None
    severity: Severity | None = None


@dataclass(frozen=True)
class QualityResult:
    dataset_id: str
    table_name: str
    check_name: str
    status: str
    observed_value: float | None
    threshold_value: float | None
    details: dict[str, object]
    severity: Severity = "error"


@dataclass(frozen=True)
class QualityRegistryEntry:
    check_name: str
    dataset_id: str
    table_name: str | None
    severity: Severity
    threshold_value: float | None
    comparator: Comparator | None
    enabled: bool


@dataclass(frozen=True)
class GateResult:
    dataset_id: str
    decision: GateDecision
    worst_severity: Severity | None
    result_count: int
    failed_count: int
    failed_results: tuple[QualityResult, ...]

    @property
    def passed(self) -> bool:
        return self.decision == "pass"


@dataclass(frozen=True)
class _SemanticContractColumn:
    table_name: str
    column_name: str
    data_type: str
    sign: str | None
    unit: str | None
    scale: str | None
    natural_key: bool | None


@dataclass(frozen=True)
class ReferentialQualityCheck:
    """Declarative "every row in ``child_table`` resolves to a parent row in
    ``parent_table``" orphan check.

    First-class replacement for the hand-written ``LEFT JOIN ... WHERE
    parent.key IS NULL`` strings duplicated across ``_check_specs`` (see e.g.
    ``orphan_equity_daily_bars``, ``xbrl_filing_facts_without_context``,
    ``xbrl_filing_dimensions_without_context``,
    ``xbrl_filing_contexts_without_sec_submission``). ``compile()`` accepts
    either the single-key convenience fields (``child_key``/``parent_key``) or
    composite key tuples (``child_keys``/``parent_keys``), then produces the
    vetted anti-join shape::

        SELECT count(*) FROM child c
        LEFT JOIN parent p ON p.<parent_key> = c.<child_key>
        WHERE c.<child_key> IS NOT NULL AND p.<parent_key> IS NULL

    A NULL child join key has nothing to resolve and is excluded from both
    sides of the count -- it is NOT an orphan, matching the existing
    ``security_id IS NOT NULL``-guarded hand-rolled checks. For composite
    joins, if any child join key is NULL, that child row is skipped.

    ``compile()`` returns a plain ``SqlQualityCheck``, so
    ``run_warehouse_quality_checks`` needs no changes to execute a
    referential check: the existing ``required_tables``/``warn_if_missing``
    machinery already no-ops the check (status ``"warning"``, not
    ``"failed"``) when ``parent_table`` (or ``child_table``) does not yet
    exist -- e.g. a forward-looking parent table a later sprint hasn't
    landed.
    """

    dataset_id: str
    check_name: str
    child_table: str
    parent_table: str
    child_key: str | None = None
    parent_key: str | None = None
    child_keys: tuple[str, ...] | None = None
    parent_keys: tuple[str, ...] | None = None
    table_name: str | None = None
    warn_if_missing: bool = True
    failure_status: FailureStatus = "failed"

    def _join_keys(self) -> tuple[tuple[str, ...], tuple[str, ...]]:
        child_keys = self.child_keys
        parent_keys = self.parent_keys
        if child_keys is None and self.child_key is not None:
            child_keys = (self.child_key,)
        if parent_keys is None and self.parent_key is not None:
            parent_keys = (self.parent_key,)
        if child_keys is None or parent_keys is None:
            raise ValueError("ReferentialQualityCheck requires child and parent join keys")
        if not child_keys or not parent_keys:
            raise ValueError("ReferentialQualityCheck join key tuples cannot be empty")
        if len(child_keys) != len(parent_keys):
            raise ValueError(
                "ReferentialQualityCheck child and parent join key tuples must be the same length"
            )
        return child_keys, parent_keys

    def compile(self) -> SqlQualityCheck:
        child_keys, parent_keys = self._join_keys()
        join_predicate = " AND ".join(
            f"p.{parent_key} = c.{child_key}"
            for child_key, parent_key in zip(child_keys, parent_keys, strict=True)
        )
        child_not_null = " AND ".join(f"c.{child_key} IS NOT NULL" for child_key in child_keys)
        sql = f"""
            SELECT count(*)::DOUBLE
            FROM {self.child_table} c
            LEFT JOIN {self.parent_table} p
              ON {join_predicate}
            WHERE {child_not_null}
              AND p.{parent_keys[0]} IS NULL
        """
        return SqlQualityCheck(
            dataset_id=self.dataset_id,
            table_name=self.table_name or self.child_table,
            check_name=self.check_name,
            sql=sql,
            threshold=0.0,
            comparator="eq",
            required_tables=(self.child_table, self.parent_table),
            warn_if_missing=self.warn_if_missing,
            failure_status=self.failure_status,
        )


for _type in (
    SqlQualityCheck,
    QualityResult,
    QualityRegistryEntry,
    GateResult,
    _SemanticContractColumn,
    ReferentialQualityCheck,
):
    _type.__module__ = "atx_db.quality"

del _type
