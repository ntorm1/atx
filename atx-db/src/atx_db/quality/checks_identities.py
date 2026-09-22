"""Tier1-S4 T6: measured quality gates over the standardized and daily-market layers.

Six specs:

* three per-filing accounting identities (balance sheet, gross profit, cash flow) with
  the spec's 0.5%-or-$1M tolerance;
* the cross-source shares test (archive vs dei within 5% for at least 95% of securities);
* derived-metric family coverage (every family in ``derived_metric_definitions`` must emit
  at least one value);
* item coverage against Sprint 2's published target.

This module is a LEAF of ``atx_db.quality``: it imports only ``._types`` at module level, so
it cannot introduce an import cycle inside the package (enforced by
``test_decomposed_package_import_graphs_are_acyclic``). The Sprint 2 coverage
constants/helpers are imported lazily inside the factory, mirroring ``_runner``'s
``signal_eval`` precedent.

Schema note, corrected after fix-round 1 review: ``fundamental_standardized`` DOES carry a
``revision_sequence`` column -- migration 0271 (``bodies_0271.py::_standardized_fundamentals_release``)
adds it via ``ALTER TABLE ... ADD COLUMN``, and the production batch writer
(``_standardization_set_based.py``) populates it going forward. An earlier version of this module
claimed the column did not exist (true only of the original ``CREATE TABLE`` in
``bodies_0001_0137.py``, before migration 0271 landed) and used a plain ``is_latest_revision``
filter with no further tie-break, mirroring ``item_coverage.load_item_coverage_inputs``. That
filter is still correct as a *scope* (it is what identifies "the currently valid fact"), but
the per-code pivot below also needs an explicit, deterministic tie-break for the rare case where
more than one ``is_latest_revision`` row can exist for the same
``(source, security_id, period_end, basis, source_accession, canonical_code)`` key (e.g. two
different ``rule_id``s mapping to the same canonical code). ``revision_sequence`` is nullable --
only the set-based writer populates it, the row-at-a-time ``standardization.py`` writer does not --
so ``available_at`` (``NOT NULL`` on every row since the original ``CREATE TABLE``) is used as the
tie-break instead of ``revision_sequence``, so the dedup works regardless of which writer produced
a given row.
"""

from __future__ import annotations

from dataclasses import dataclass

from ._types import Severity, SqlQualityCheck

IDENTITY_TOLERANCE_PCT = 0.005
IDENTITY_TOLERANCE_ABS = 1_000_000.0
SHARES_TOLERANCE = 0.05
SHARES_PASS_RATE = 0.95

SHARES_CROSS_SOURCE_CHECK_NAME = "shares_cross_source_disagreement"
DERIVED_FAMILY_COVERAGE_CHECK_NAME = "derived_metric_families_without_values"
ITEM_COVERAGE_CHECK_NAME = "fundamental_item_coverage_below_target"


@dataclass(frozen=True)
class AccountingIdentity:
    """One per-filing identity, expressed over pivoted canonical codes.

    ``lhs``/``rhs``/``scale`` are SQL fragments over the pivoted column names, which are
    exactly the canonical codes in ``codes``. ``codes`` also drives the NOT NULL guard: a
    filing missing any required code is skipped, never counted as a violation. Optional
    codes are the ones absent from ``codes`` but referenced through ``coalesce`` in the
    fragments.
    """

    check_name: str
    codes: tuple[str, ...]
    lhs: str
    rhs: str
    scale: str
    severity: Severity
    description: str


ACCOUNTING_IDENTITIES: tuple[AccountingIdentity, ...] = (
    AccountingIdentity(
        check_name="balance_sheet_identity_violations",
        codes=("total_assets", "total_liabilities", "stockholders_equity"),
        lhs="total_assets",
        rhs="total_liabilities + stockholders_equity + coalesce(minority_interest_bs, 0)",
        scale="total_assets",
        severity="error",
        description="assets = liabilities + equity (+ minority interest)",
    ),
    AccountingIdentity(
        check_name="gross_profit_identity_violations",
        codes=("gross_profit__1004", "revenue", "cost_of_revenue_cogs"),
        lhs="gross_profit__1004",
        rhs="revenue - cost_of_revenue_cogs",
        scale="revenue",
        severity="error",
        description="gross profit = revenue - cost of revenue",
    ),
    AccountingIdentity(
        check_name="cash_flow_identity_violations",
        codes=(
            "cash_flow_from_operations",
            "cash_flow_from_investing",
            "cash_flow_from_financing",
            "net_change_in_cash",
        ),
        lhs=(
            "cash_flow_from_operations + cash_flow_from_investing + cash_flow_from_financing "
            "+ coalesce(fx_effect_on_cash, 0)"
        ),
        rhs="net_change_in_cash",
        scale="net_change_in_cash",
        severity="error",
        description="cfo + cfi + cff + fx = change in cash",
    ),
)

# Every code any identity can reference, pivoted once.
_PIVOT_CODES: tuple[str, ...] = (
    "cash_flow_from_financing",
    "cash_flow_from_investing",
    "cash_flow_from_operations",
    "cost_of_revenue_cogs",
    "fx_effect_on_cash",
    "gross_profit__1004",
    "minority_interest_bs",
    "net_change_in_cash",
    "revenue",
    "stockholders_equity",
    "total_assets",
    "total_liabilities",
)


def identity_violation_sql(identity: AccountingIdentity, *, source: str) -> str:
    """Count filings whose ``identity`` breaks by more than 0.5% of scale or $1M.

    The pivot keys on ``(security_id, period_end, basis, source_accession)`` -- one filing's
    view of one period, scoped to a single pinned ``source`` the same way
    ``item_coverage.load_item_coverage_inputs`` pins its own ``source`` filter (fix-round-1
    finding 3: without this, two rows from different ``source`` values loaded into
    ``fundamental_standardized`` could both carry ``is_latest_revision=true`` for the same
    filing and silently blend across sources in the pivot). Within that single source, a
    per-code ``arg_max(value, available_at)`` -- rather than a plain ``max(value)``, which
    would pick whichever row's VALUE happens to be numerically larger, not the more recent
    one -- makes the "which duplicate wins" tie-break explicit and deterministic if more than
    one ``is_latest_revision`` row is ever found for the same
    ``(source, security_id, period_end, basis, source_accession, canonical_code)`` key (e.g.
    two ``rule_id``s producing the same canonical code). ``available_at`` is used as the
    tie-break rather than the table's own ``revision_sequence`` column because
    ``revision_sequence`` is only populated by the set-based production writer
    (``_standardization_set_based.py``) and is NULL for rows written through the per-row
    ``standardization.py`` path, whereas ``available_at`` is ``NOT NULL`` on every row.
    """

    code_list = ", ".join(f"'{code}'" for code in _PIVOT_CODES)
    pivot = ",\n            ".join(
        f"arg_max(CASE WHEN canonical_code = '{code}' THEN value END, "
        f"CASE WHEN canonical_code = '{code}' THEN available_at END) AS {code}"
        for code in _PIVOT_CODES
    )
    present = " AND ".join(f"{code} IS NOT NULL" for code in identity.codes)
    return f"""
        WITH latest AS (
            SELECT
                security_id,
                period_end,
                basis,
                source_accession,
                canonical_code,
                value,
                available_at
            FROM fundamental_standardized
            WHERE source = '{source}'
              AND is_latest_revision
              AND value IS NOT NULL
              AND canonical_code IN ({code_list})
        ),
        wide AS (
            SELECT
                security_id,
                period_end,
                basis,
                source_accession,
                {pivot}
            FROM latest
            GROUP BY security_id, period_end, basis, source_accession
        )
        SELECT count(*)::DOUBLE
        FROM wide
        WHERE {present}
          AND abs(({identity.lhs}) - ({identity.rhs}))
              > greatest(abs({identity.scale}) * {IDENTITY_TOLERANCE_PCT},
                         {IDENTITY_TOLERANCE_ABS})
    """


# Fraction of two-source securities whose median |dei/archive - 1| exceeds the tolerance.
# The spec asks for "within 5% for at least 95% of securities", so the observed value is
# the FAILING fraction and the threshold is 1 - 0.95 = 0.05.
#
# Fix-round-1 finding 2: when NO security has both a dei and an archive share count (an
# empty ``per_security``), the observed value is 1.0 -- a value that always violates the
# ``le`` threshold below 1.0 -- rather than 0.0. An empty ``per_security`` means either the
# table is empty or the reconciliation pipeline that populates
# ``shares_reconciliation_ratio`` broke upstream; either way that is "no evidence this check
# passed," which must fail loudly rather than read as a clean pass on zero measured rows
# (the same "empty anti-join counts as success" failure mode the survivorship gate was
# built to rule out).
_SHARES_SQL = f"""
WITH per_security AS (
    SELECT
        security_id,
        median(abs(shares_reconciliation_ratio - 1.0)) AS median_abs_gap
    FROM market_daily_metrics
    WHERE is_latest_revision
      AND shares_reconciliation_ratio IS NOT NULL
      AND shares_reconciliation_ratio > 0
    GROUP BY security_id
)
SELECT
    CASE
        WHEN count(*) = 0 THEN 1.0
        ELSE (count(*) FILTER (WHERE median_abs_gap > {SHARES_TOLERANCE}))::DOUBLE / count(*)
    END
FROM per_security
"""

_DERIVED_FAMILY_SQL = """
SELECT count(*)::DOUBLE
FROM (
    SELECT d.family
    FROM derived_metric_definitions d
    LEFT JOIN (
        SELECT DISTINCT metric_code
        FROM derived_metric_values
        WHERE is_latest_revision AND value_status = 'valid'
          AND value IS NOT NULL AND isfinite(value)
          AND history_status = 'event_reconstructed'
    ) v ON v.metric_code = d.metric_code
    GROUP BY d.family
    HAVING count(v.metric_code) = 0
)
"""


def _item_coverage_sql(
    *,
    source: str,
    universe_id: str,
    target_items: int,
    target_pct: float,
    minimum_fiscal_year: int,
    basis: str,
) -> str:
    """Use the exact provider gate, including required years and cohort evidence."""
    from ..item_coverage import coverage_gate_count_sql

    count_sql = coverage_gate_count_sql(
        source=source, universe_id=universe_id, minimum_fiscal_year=minimum_fiscal_year,
        target_pct=target_pct, basis=basis,
    )
    return f"SELECT greatest(0, {target_items} - ( {count_sql} ))::DOUBLE"


def identity_check_specs(**_ignored: object) -> tuple[SqlQualityCheck, ...]:
    """The six Tier-1 measured gates.

    Accepts and ignores the ``daily_macro_stale_days`` / ``monthly_macro_stale_days`` /
    ``valuation_stale_gap_days`` common kwargs so the factory is interchangeable with the
    other ``*_check_specs`` factories.
    """

    from ..item_coverage import (
        DEFAULT_SOURCE,
        DEFAULT_UNIVERSE_ID,
        ITEM_COVERAGE_GATE_BASIS,
        ITEM_COVERAGE_TARGET_ITEMS,
        ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR,
        ITEM_COVERAGE_TARGET_PCT,
    )

    specs: list[SqlQualityCheck] = [
        SqlQualityCheck(
            dataset_id="fundamental_standardized",
            table_name="fundamental_standardized",
            check_name=identity.check_name,
            sql=identity_violation_sql(identity, source=DEFAULT_SOURCE),
            threshold=0.0,
            comparator="le",
            required_tables=("fundamental_standardized",),
            warn_if_missing=True,
            failure_status="failed",
            severity=identity.severity,
        )
        for identity in ACCOUNTING_IDENTITIES
    ]
    specs.append(
        SqlQualityCheck(
            dataset_id="market_daily",
            table_name="market_daily_metrics",
            check_name=SHARES_CROSS_SOURCE_CHECK_NAME,
            sql=_SHARES_SQL,
            threshold=round(1.0 - SHARES_PASS_RATE, 10),
            comparator="le",
            required_tables=("market_daily_metrics",),
            warn_if_missing=True,
            failure_status="failed",
            severity="error",
        )
    )
    specs.append(
        SqlQualityCheck(
            dataset_id="derived_metrics",
            table_name="derived_metric_values",
            check_name=DERIVED_FAMILY_COVERAGE_CHECK_NAME,
            sql=_DERIVED_FAMILY_SQL,
            threshold=0.0,
            comparator="le",
            required_tables=("derived_metric_definitions", "derived_metric_values"),
            warn_if_missing=True,
            failure_status="warning",
            severity="warning",
        )
    )
    specs.append(
        SqlQualityCheck(
            dataset_id="fundamental_item_coverage",
            table_name="fundamental_item_coverage",
            check_name=ITEM_COVERAGE_CHECK_NAME,
            sql=_item_coverage_sql(
                source=DEFAULT_SOURCE,
                universe_id=DEFAULT_UNIVERSE_ID,
                target_items=ITEM_COVERAGE_TARGET_ITEMS,
                target_pct=ITEM_COVERAGE_TARGET_PCT,
                minimum_fiscal_year=ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR,
                basis=ITEM_COVERAGE_GATE_BASIS,
            ),
            threshold=0.0,
            comparator="le",
            required_tables=("fundamental_item_coverage", "item_coverage_cohort_years"),
            warn_if_missing=True,
            failure_status="warning",
            severity="warning",
        )
    )
    return tuple(specs)
