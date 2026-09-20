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
``test_decomposed_package_import_graphs_are_acyclic``). The Sprint 2 coverage constants are
imported lazily inside the factory, mirroring ``_runner``'s ``signal_eval`` precedent.

Substitution note (verified against ``fundamental_standardized``'s actual schema in
``migrations/bodies_0001_0137.py::_fundamental_standardized_schema_catalog``): the table has
no ``revision_sequence`` column (that column exists on ``fundamental_points`` /
``fundamental_statement_points`` / TTM tables, not here). The plan's identity SQL sketch
assumed one. Sprint 2's own reader, ``item_coverage.load_item_coverage_inputs``, resolves
"the current fact" for this table with a plain ``WHERE is_latest_revision`` filter and no
further tie-break; the identity pivot below follows that precedent instead of an
``arg_max(..., revision_sequence)`` that would not compile.
"""

from __future__ import annotations

from dataclasses import dataclass

from ._types import SqlQualityCheck

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
    severity: str
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


def identity_violation_sql(identity: AccountingIdentity) -> str:
    """Count filings whose ``identity`` breaks by more than 0.5% of scale or $1M.

    The pivot keys on ``(security_id, period_end, basis, source_accession)`` -- one filing's
    view of one period. ``fundamental_standardized`` carries no ``revision_sequence`` column
    (unlike ``fundamental_points``/TTM tables), so "the current fact" is resolved the same
    way Sprint 2's ``item_coverage.load_item_coverage_inputs`` resolves it: a plain
    ``WHERE is_latest_revision`` filter, with no further tie-break.
    """

    code_list = ", ".join(f"'{code}'" for code in _PIVOT_CODES)
    pivot = ",\n            ".join(
        f"max(CASE WHEN canonical_code = '{code}' THEN value END) AS {code}" for code in _PIVOT_CODES
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
                value
            FROM fundamental_standardized
            WHERE is_latest_revision
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
        WHEN count(*) = 0 THEN 0.0
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
        WHERE is_latest_revision
    ) v ON v.metric_code = d.metric_code
    GROUP BY d.family
    HAVING count(v.metric_code) = 0
)
"""


def _item_coverage_sql(*, target_items: int, target_pct: float, minimum_fiscal_year: int) -> str:
    """How many items short of the published target the warehouse is.

    An item counts only when its coverage clears ``target_pct`` in EVERY in-scope fiscal
    year -- the same all-years rule ``item_coverage.evaluate_item_coverage_gate`` applies,
    so the gate and the published ITEM_COVERAGE.md can never disagree.
    """

    return f"""
        SELECT greatest(0, {target_items} - count(*))::DOUBLE
        FROM (
            SELECT item_id
            FROM fundamental_item_coverage
            WHERE fiscal_year >= {minimum_fiscal_year}
            GROUP BY item_id
            HAVING min(coverage_pct) >= {target_pct}
        )
    """


def identity_check_specs(**_ignored: object) -> tuple[SqlQualityCheck, ...]:
    """The six Tier-1 measured gates.

    Accepts and ignores the ``daily_macro_stale_days`` / ``monthly_macro_stale_days`` /
    ``valuation_stale_gap_days`` common kwargs so the factory is interchangeable with the
    other ``*_check_specs`` factories.
    """

    from ..item_coverage import (
        ITEM_COVERAGE_TARGET_ITEMS,
        ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR,
        ITEM_COVERAGE_TARGET_PCT,
    )

    specs: list[SqlQualityCheck] = [
        SqlQualityCheck(
            dataset_id="fundamental_standardized",
            table_name="fundamental_standardized",
            check_name=identity.check_name,
            sql=identity_violation_sql(identity),
            threshold=0.0,
            comparator="le",
            required_tables=("fundamental_standardized",),
            warn_if_missing=True,
            failure_status="failed",
            severity=identity.severity,  # type: ignore[arg-type]
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
                target_items=ITEM_COVERAGE_TARGET_ITEMS,
                target_pct=ITEM_COVERAGE_TARGET_PCT,
                minimum_fiscal_year=ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR,
            ),
            threshold=0.0,
            comparator="le",
            required_tables=("fundamental_item_coverage",),
            warn_if_missing=True,
            failure_status="warning",
            severity="warning",
        )
    )
    return tuple(specs)
