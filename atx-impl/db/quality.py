from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from typing import Iterable, Literal, Mapping, Sequence

from .connection import DuckDBStore
from .lake import DEFAULT_EXPORT_OBJECTS
from .schema_contract import (
    ColumnSpec,
    PIT_COLUMN_NAMES,
    _fetch_catalogued_tables,
    _fetch_live_tables,
    build_contract_manifest,
)
from .warehouse import quality_check


# security_identifier_history.internal_cusip is internal-only matching support
# (see the field_catalog description seeded in migration 0079) and must never
# appear in a lake-exported / public / catalogued-public object. This is the
# single column the boundary protects today; the export-scan check below
# fails if any DEFAULT_EXPORT_OBJECTS member ever carries a column by this
# name.
INTERNAL_ONLY_EXPORT_FORBIDDEN_COLUMN = "internal_cusip"
DEFAULT_VALUATION_STALE_GAP_DAYS = 5

# PF2-S1 S1-1: check_name constants for the two schema-contract gates below, so tests
# and (later) PF2-S10's orchestrator gating never have to hand-copy the literal string.
CATALOG_COMPLETENESS_CHECK_NAME = "catalog_completeness"
PIT_COLUMN_PRESENCE_CHECK_NAME = "pit_column_presence"
SCHEMA_CONTRACT_DATASET_ID = "schema_contract"

# PF-S7 S7-1: the statement-layer parent that fundamental_ratios rows resolve
# against. TODAY this is fundamental_points (raw SEC companyfacts). Once
# PF-S3/PF-S8 promote fundamental_statement_points to the ratio engine's
# primary statement-layer input, repoint this constant (or add a second
# ReferentialQualityCheck alongside the one that uses it in
# _referential_check_specs) -- a one-line upgrade.
PARENT_TABLE_FOR_FUNDAMENTAL_RATIOS = "fundamental_points"


def _export_scan_internal_cusip_sql(export_objects: tuple[str, ...]) -> str:
    """Build the export-scan SQL: count DEFAULT_EXPORT_OBJECTS columns named
    ``internal_cusip``. Metadata-only (``duckdb_columns()``), so it is safe to
    run even against a warehouse where some listed export objects do not
    exist yet (an empty/partial test DB) -- the count is simply 0 for those.
    """
    if not export_objects:
        return "SELECT 0.0"
    placeholders = ", ".join(f"'{name}'" for name in export_objects)
    return f"""
        SELECT count(*)::DOUBLE
        FROM duckdb_columns()
        WHERE schema_name = 'main'
          AND table_name IN ({placeholders})
          AND column_name = '{INTERNAL_ONLY_EXPORT_FORBIDDEN_COLUMN}'
    """


Comparator = Literal["eq", "le", "ge"]
FailureStatus = Literal["failed", "warning"]
# PF2-S1 S1-1: a gate-readiness tag, orthogonal to failure_status/status. "critical"
# marks a check PF2-S10 can later wire into orchestrator gating (clause G, adopted
# incrementally); this sprint only authors that tag, it does not wire any gate.
# Defaults to "standard" everywhere so every pre-existing check (and QualityResult
# construction site) is unaffected -- additive only.
Severity = Literal["standard", "critical"]


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
    severity: Severity = "standard"


@dataclass(frozen=True)
class QualityResult:
    dataset_id: str
    table_name: str
    check_name: str
    status: str
    observed_value: float | None
    threshold_value: float | None
    details: dict[str, object]
    severity: Severity = "standard"


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


def _passes(observed_value: float, threshold: float, comparator: Comparator) -> bool:
    if comparator == "eq":
        return observed_value == threshold
    if comparator == "le":
        return observed_value <= threshold
    if comparator == "ge":
        return observed_value >= threshold
    raise ValueError(f"Unknown comparator {comparator!r}")


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
    single_table_checks = (
        SqlQualityCheck(
            dataset_id="fundamental_standardized",
            table_name="v_fundamental_standardization_coverage",
            check_name="fundamental_standardization_exception_rate",
            sql="""
                SELECT coalesce(max(
                    CASE
                        WHEN populated_item_count + exception_count = 0 THEN 0.0
                        ELSE exception_count / (populated_item_count + exception_count)
                    END
                ), 0.0)::DOUBLE
                FROM v_fundamental_standardization_coverage
            """,
            threshold=0.25,
            comparator="le",
            required_tables=(
                "fundamental_standardized",
                "fundamental_standardization_exception",
                "v_fundamental_standardization_coverage",
            ),
            detail_sql="""
                SELECT
                    source,
                    security_id,
                    period_end,
                    basis,
                    populated_item_count,
                    exception_count,
                    CASE
                        WHEN populated_item_count + exception_count = 0 THEN 0.0
                        ELSE exception_count / (populated_item_count + exception_count)
                    END AS exception_rate
                FROM v_fundamental_standardization_coverage
                WHERE populated_item_count + exception_count > 0
                  AND exception_count / (populated_item_count + exception_count) > 0.25
                ORDER BY exception_rate DESC, security_id, period_end
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_standardized",
            table_name="v_fundamental_standardization_coverage",
            check_name="fundamental_standardization_template_coverage",
            sql="""
                SELECT coalesce(min(coverage_ratio), 1.0)::DOUBLE
                FROM v_fundamental_standardization_coverage
                WHERE populated_item_count + exception_count > 0
            """,
            threshold=0.05,
            comparator="ge",
            required_tables=("fundamental_standardized", "v_fundamental_standardization_coverage"),
            detail_sql="""
                SELECT
                    source,
                    security_id,
                    period_end,
                    basis,
                    populated_item_count,
                    template_item_count,
                    coverage_ratio,
                    exception_count
                FROM v_fundamental_standardization_coverage
                WHERE populated_item_count + exception_count > 0
                  AND coverage_ratio < 0.05
                ORDER BY coverage_ratio, security_id, period_end
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="tbltickerhistory_daily",
            table_name="equity_daily_bars",
            check_name="duplicate_equity_daily_bars",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, security_id, trade_date, count(*) AS row_count
                    FROM equity_daily_bars
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("equity_daily_bars",),
        ),
        SqlQualityCheck(
            dataset_id="tbltickerhistory_daily",
            table_name="equity_daily_bars",
            check_name="bad_ohlcv_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM equity_daily_bars
                WHERE volume < 0
                   OR open <= 0
                   OR high <= 0
                   OR low <= 0
                   OR close <= 0
                   OR high < greatest(open, low, close)
                   OR low > least(open, high, close)
            """,
            threshold=0.0,
            required_tables=("equity_daily_bars",),
        ),
        SqlQualityCheck(
            dataset_id="tbltickerhistory_daily",
            table_name="equity_daily_bars",
            check_name="orphan_equity_daily_bars",
            sql="""
                SELECT count(*)::DOUBLE
                FROM equity_daily_bars b
                LEFT JOIN securities s ON s.security_id = b.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("equity_daily_bars", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="tbltickerhistory_daily",
            table_name="equity_daily_bars",
            check_name="missing_bar_available_at",
            sql="SELECT count(*)::DOUBLE FROM equity_daily_bars WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("equity_daily_bars",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_actions",
            table_name="corporate_actions",
            check_name="duplicate_corporate_actions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        action_type,
                        ex_date,
                        coalesce(details_json, '') AS details_json,
                        count(*) AS row_count
                    FROM corporate_actions
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("corporate_actions",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_actions",
            table_name="corporate_actions",
            check_name="bad_corporate_action_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_actions
                WHERE ex_date IS NULL
                   OR security_id IS NULL
                   OR action_type IS NULL
                   OR (cash_amount IS NOT NULL AND cash_amount < 0)
                   OR (adjustment_factor IS NOT NULL AND adjustment_factor <= 0)
            """,
            threshold=0.0,
            required_tables=("corporate_actions",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_actions",
            table_name="corporate_actions",
            check_name="orphan_corporate_actions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_actions c
                LEFT JOIN securities s ON s.security_id = c.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("corporate_actions", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="adjustment_factor_history",
            table_name="corp_action_type_dim",
            check_name="bad_corp_action_type_dim_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corp_action_type_dim
                WHERE type_code IS NULL
                   OR event_type IS NULL
                   OR event_type = ''
                   OR category IS NULL
                   OR category = ''
                   OR sub_category IS NULL
                   OR sub_category = ''
                   OR description IS NULL
                   OR description = ''
                   OR affects_price IS NULL
                   OR affects_shares IS NULL
                   OR mandatory IS NULL
            """,
            threshold=0.0,
            required_tables=("corp_action_type_dim",),
        ),
        SqlQualityCheck(
            dataset_id="adjustment_factor_history",
            table_name="adjustment_factor_history",
            check_name="duplicate_adjustment_factor_history",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT adjustment_factor_id, count(*) AS row_count
                    FROM adjustment_factor_history
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("adjustment_factor_history",),
        ),
        SqlQualityCheck(
            dataset_id="adjustment_factor_history",
            table_name="adjustment_factor_history",
            check_name="bad_adjustment_factor_history_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM adjustment_factor_history a
                LEFT JOIN corp_action_type_dim d ON d.type_code = a.type_code
                WHERE a.adjustment_factor_id IS NULL
                   OR a.adjustment_factor_id = ''
                   OR a.source IS NULL
                   OR a.source = ''
                   OR a.source_action_source IS NULL
                   OR a.source_action_source = ''
                   OR a.security_id IS NULL
                   OR a.security_id = ''
                   OR a.ex_date IS NULL
                   OR a.event_type IS NULL
                   OR a.event_type = ''
                   OR a.classification_reason IS NULL
                   OR a.classification_reason = ''
                   OR d.type_code IS NULL
                   OR d.event_type <> a.event_type
                   OR a.event_ref_id IS NULL
                   OR a.event_ref_id = ''
                   OR a.factor_price <= 0
                   OR a.factor_shares <= 0
                   OR a.factor_volume <= 0
                   OR a.cumulative_price_factor <= 0
                   OR a.cumulative_share_factor <= 0
                   OR a.available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("adjustment_factor_history", "corp_action_type_dim"),
        ),
        SqlQualityCheck(
            dataset_id="adjustment_factor_history",
            table_name="adjustment_factor_history",
            check_name="orphan_adjustment_factor_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM adjustment_factor_history a
                LEFT JOIN securities s ON s.security_id = a.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("adjustment_factor_history", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="daily_adjustment_factors",
            table_name="daily_adjustment_factors",
            check_name="duplicate_daily_adjustment_factors",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        bar_source,
                        factor_source,
                        security_id,
                        trade_date,
                        as_of_date,
                        count(*) AS row_count
                    FROM daily_adjustment_factors
                    GROUP BY 1, 2, 3, 4, 5, 6
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("daily_adjustment_factors",),
        ),
        SqlQualityCheck(
            dataset_id="daily_adjustment_factors",
            table_name="daily_adjustment_factors",
            check_name="bad_daily_adjustment_factor_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM daily_adjustment_factors d
                WHERE d.daily_adjustment_id IS NULL
                   OR d.daily_adjustment_id = ''
                   OR d.source IS NULL
                   OR d.source = ''
                   OR d.bar_source IS NULL
                   OR d.bar_source = ''
                   OR d.factor_source IS NULL
                   OR d.factor_source = ''
                   OR d.security_id IS NULL
                   OR d.security_id = ''
                   OR d.trade_date IS NULL
                   OR d.as_of_date IS NULL
                   OR d.as_of_date < d.trade_date
                   OR d.split_price_factor <= 0
                   OR d.split_share_factor <= 0
                   OR d.dividend_total_return_factor <= 0
                   OR d.total_return_price_factor <= 0
                   OR d.raw_close <= 0
                   OR d.split_adjusted_close <= 0
                   OR d.total_return_adjusted_close <= 0
                   OR d.visible_event_count < 0
                   OR d.split_event_count < 0
                   OR d.cash_div_event_count < 0
                   OR d.available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("daily_adjustment_factors",),
        ),
        SqlQualityCheck(
            dataset_id="daily_adjustment_factors",
            table_name="daily_adjustment_factors",
            check_name="orphan_daily_adjustment_factor_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM daily_adjustment_factors d
                LEFT JOIN securities s ON s.security_id = d.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("daily_adjustment_factors", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="shares_outstanding_history",
            table_name="shares_outstanding_history",
            check_name="duplicate_shares_outstanding_history",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT share_history_id, count(*) AS row_count
                    FROM shares_outstanding_history
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("shares_outstanding_history",),
        ),
        SqlQualityCheck(
            dataset_id="shares_outstanding_history",
            table_name="shares_outstanding_history",
            check_name="bad_shares_outstanding_history_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM shares_outstanding_history
                WHERE share_history_id IS NULL
                   OR share_history_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR share_count_type NOT IN ('shares_outstanding', 'shares_basic_avg', 'shares_diluted_avg')
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR period_type NOT IN ('instant', 'duration')
                   OR period_end IS NULL
                   OR effective_date IS NULL
                   OR effective_date <> period_end
                   OR as_of_date IS NULL
                   OR period_end > as_of_date
                   OR available_at IS NULL
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR share_count < 0
                   OR source_url IS NULL
                   OR source_url = ''
            """,
            threshold=0.0,
            required_tables=("shares_outstanding_history",),
        ),
        SqlQualityCheck(
            dataset_id="shares_outstanding_history",
            table_name="shares_outstanding_history",
            check_name="orphan_shares_outstanding_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM shares_outstanding_history h
                LEFT JOIN securities s ON s.security_id = h.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("shares_outstanding_history", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="duplicate_identifier_history_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, id_type, id_value, valid_from, count(*) AS row_count
                    FROM security_identifier_history
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("security_identifier_history",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="bad_identifier_validity_ranges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_identifier_history
                WHERE valid_to IS NOT NULL
                  AND valid_to <= valid_from
            """,
            threshold=0.0,
            required_tables=("security_identifier_history",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="orphan_identifier_history",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_identifier_history i
                LEFT JOIN securities s ON s.security_id = i.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("security_identifier_history", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="identifier_multi_security_overlaps",
            # Ticker/identifier uniqueness-per-window is a reference-master
            # invariant for security-level identifiers. ENTITY_ID is above
            # security_id and may be shared after M&A. The raw tbltickerhistory
            # price feed legitimately violates
            # it (share classes trade concurrently, tickers are recycled across
            # issuers, missing vendor ids), the same way CRSP keys on PERMNO not
            # ticker — so price-feed securities are excluded here. Intra-security
            # duplicate intervals are still caught by the self-overlap check below.
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_identifier_history a
                JOIN security_identifier_history b
                  ON a.id_type = b.id_type
                 AND a.id_value = b.id_value
                 AND a.security_id <> b.security_id
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
                 AND a.security_id < b.security_id
                WHERE a.id_type <> 'ENTITY_ID'
                  AND a.source NOT LIKE 'tbltickerhistory%'
                  AND b.source NOT LIKE 'tbltickerhistory%'
            """,
            threshold=0.0,
            required_tables=("security_identifier_history",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="identifier_same_source_self_overlaps",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_identifier_history a
                JOIN security_identifier_history b
                  ON a.security_id = b.security_id
                 AND a.id_type = b.id_type
                 AND a.id_value = b.id_value
                 AND a.source = b.source
                 AND a.valid_from < b.valid_from
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
            """,
            threshold=0.0,
            required_tables=("security_identifier_history",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="security_identifier_history",
            check_name="export_scan_internal_cusip_leak",
            # Enforces the 13f_holdings.md B.3 boundary: internal_cusip (see
            # migration 0079's field_catalog note) is internal-only matching
            # support and must never appear on any lake-exported / public /
            # catalogued-public object. Today the boundary is enforced by
            # OMISSION from lake.DEFAULT_EXPORT_OBJECTS; this check turns that
            # into an enforced invariant instead of a tribal-knowledge
            # omission -- it fails if internal_cusip is ever added as a
            # column on any exported object.
            sql=_export_scan_internal_cusip_sql(DEFAULT_EXPORT_OBJECTS),
            threshold=0.0,
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="exchange_listings",
            check_name="orphan_exchange_listings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM exchange_listings l
                LEFT JOIN securities s ON s.security_id = l.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("exchange_listings", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="exchange_listings",
            check_name="listing_multi_security_overlaps",
            # See identifier_multi_security_overlaps: ticker uniqueness is a
            # reference-master invariant, not a raw price-feed one, so
            # tbltickerhistory securities are excluded.
            sql="""
                SELECT count(*)::DOUBLE
                FROM exchange_listings a
                JOIN exchange_listings b
                  ON a.ticker = b.ticker
                 AND coalesce(a.exchange_code, '') = coalesce(b.exchange_code, '')
                 AND a.security_id <> b.security_id
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
                 AND a.security_id < b.security_id
                WHERE a.source NOT LIKE 'tbltickerhistory%'
                  AND b.source NOT LIKE 'tbltickerhistory%'
            """,
            threshold=0.0,
            required_tables=("exchange_listings",),
        ),
        SqlQualityCheck(
            dataset_id="sec_security_master",
            table_name="exchange_listings",
            check_name="listing_same_source_self_overlaps",
            sql="""
                SELECT count(*)::DOUBLE
                FROM exchange_listings a
                JOIN exchange_listings b
                  ON a.security_id = b.security_id
                 AND a.ticker = b.ticker
                 AND coalesce(a.exchange_code, '') = coalesce(b.exchange_code, '')
                 AND a.source = b.source
                 AND a.valid_from < b.valid_from
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
            """,
            threshold=0.0,
            required_tables=("exchange_listings",),
        ),
        SqlQualityCheck(
            dataset_id="nasdaq_listing_events",
            table_name="nasdaq_listing_events",
            check_name="duplicate_listing_event_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT event_id, count(*) AS row_count
                    FROM nasdaq_listing_events
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("nasdaq_listing_events",),
        ),
        SqlQualityCheck(
            dataset_id="nasdaq_listing_events",
            table_name="nasdaq_listing_events",
            check_name="bad_listing_event_required_fields",
            sql="""
                SELECT count(*)::DOUBLE
                FROM nasdaq_listing_events
                WHERE symbol IS NULL
                   OR symbol = ''
                   OR effective_date IS NULL
                   OR as_of_date IS NULL
                   OR source_url IS NULL
            """,
            threshold=0.0,
            required_tables=("nasdaq_listing_events",),
        ),
        SqlQualityCheck(
            dataset_id="nasdaq_listing_events",
            table_name="nasdaq_listing_events",
            check_name="bad_listing_event_actions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM nasdaq_listing_events
                WHERE coalesce(nasdaq_action, '') NOT IN ('', 'Add', 'Delete')
                   OR coalesce(bx_action, '') NOT IN ('', 'Add', 'Delete')
                   OR coalesce(psx_action, '') NOT IN ('', 'Add', 'Delete')
            """,
            threshold=0.0,
            required_tables=("nasdaq_listing_events",),
        ),
        SqlQualityCheck(
            dataset_id="nasdaq_listing_events",
            table_name="nasdaq_listing_events",
            check_name="listing_event_future_asof",
            sql="""
                SELECT count(*)::DOUBLE
                FROM nasdaq_listing_events
                WHERE as_of_date > current_date
            """,
            threshold=0.0,
            required_tables=("nasdaq_listing_events",),
        ),
        SqlQualityCheck(
            dataset_id="listing_status_intervals",
            table_name="listing_status_intervals",
            check_name="duplicate_listing_status_interval_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT listing_status_id, count(*) AS row_count
                    FROM listing_status_intervals
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("listing_status_intervals",),
        ),
        SqlQualityCheck(
            dataset_id="listing_status_intervals",
            table_name="listing_status_intervals",
            check_name="bad_listing_status_intervals",
            sql="""
                SELECT count(*)::DOUBLE
                FROM listing_status_intervals
                WHERE symbol IS NULL
                   OR symbol = ''
                   OR status NOT IN ('active', 'inactive')
                   OR valid_from IS NULL
                   OR (valid_to IS NOT NULL AND valid_to <= valid_from)
                   OR as_of_date IS NULL
                   OR source IS NULL
                   OR source = ''
                   OR evidence_source IS NULL
                   OR evidence_source = ''
                   OR method IS NULL
                   OR method = ''
            """,
            threshold=0.0,
            required_tables=("listing_status_intervals",),
        ),
        SqlQualityCheck(
            dataset_id="listing_status_intervals",
            table_name="listing_status_intervals",
            check_name="orphan_listing_status_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM listing_status_intervals l
                LEFT JOIN securities s
                  ON s.security_id = l.security_id
                WHERE l.security_id IS NOT NULL
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("listing_status_intervals", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="listing_status_intervals",
            table_name="listing_status_intervals",
            check_name="listing_status_same_method_overlaps",
            sql="""
                SELECT count(*)::DOUBLE
                FROM listing_status_intervals a
                JOIN listing_status_intervals b
                  ON a.source = b.source
                 AND a.evidence_source = b.evidence_source
                 AND a.method = b.method
                 AND a.symbol = b.symbol
                 AND coalesce(a.listing_venue_code, '') = coalesce(b.listing_venue_code, '')
                 AND a.status = b.status
                 AND a.listing_status_id < b.listing_status_id
                 AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
                 AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
            """,
            threshold=0.0,
            required_tables=("listing_status_intervals",),
        ),
        SqlQualityCheck(
            dataset_id="delist_code_dim",
            table_name="delist_code_dim",
            check_name="bad_delist_code_dim_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delist_code_dim
                WHERE delist_code IS NULL
                   OR delist_code = ''
                   OR code_system IS NULL
                   OR code_system = ''
                   OR reason_category IS NULL
                   OR reason_category = ''
                   OR description IS NULL
                   OR description = ''
                   OR imputation_policy IS NULL
                   OR imputation_policy = ''
                   OR source IS NULL
                   OR source = ''
                   OR (
                       default_imputed_return IS NOT NULL
                       AND (default_imputed_return < -1.0 OR default_imputed_return > 10.0)
                   )
            """,
            threshold=0.0,
            required_tables=("delist_code_dim",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="duplicate_delisting_events",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        listing_status_source,
                        source_listing_status_id,
                        delist_code,
                        count(*) AS row_count
                    FROM delisting_events
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("delisting_events",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="bad_delisting_event_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                WHERE d.delisting_event_id IS NULL
                   OR d.delisting_event_id = ''
                   OR d.source IS NULL
                   OR d.source = ''
                   OR d.listing_status_source IS NULL
                   OR d.listing_status_source = ''
                   OR d.source_listing_status_id IS NULL
                   OR d.source_listing_status_id = ''
                   OR d.symbol IS NULL
                   OR d.symbol = ''
                   OR d.delist_date IS NULL
                   OR d.as_of_date IS NULL
                   OR d.available_at IS NULL
                   OR d.delist_code IS NULL
                   OR d.delist_code = ''
                   OR d.delist_reason IS NULL
                   OR d.delist_reason = ''
                   OR d.delisting_return_type IS NULL
                   OR d.delisting_return_type = ''
                   OR d.return_policy IS NULL
                   OR d.return_policy = ''
                   OR d.return_confidence NOT IN ('none', 'low', 'medium', 'high')
                   OR d.evidence_confidence NOT IN ('low', 'medium', 'high')
                   OR d.evidence_source IS NULL
                   OR d.evidence_source = ''
                   OR d.evidence_source_table IS NULL
                   OR d.evidence_source_table = ''
                   OR d.method IS NULL
                   OR d.method = ''
                   OR (
                       d.delisting_return IS NOT NULL
                       AND (d.delisting_return < -1.0 OR d.delisting_return > 10.0)
                   )
                   OR (d.delisting_return IS NOT NULL AND d.return_confidence = 'none')
                   OR (d.is_return_imputed = true AND d.return_policy = 'none')
            """,
            threshold=0.0,
            required_tables=("delisting_events",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="orphan_delisting_event_codes",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                LEFT JOIN delist_code_dim c
                  ON c.delist_code = d.delist_code
                WHERE c.delist_code IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_events", "delist_code_dim"),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="orphan_delisting_event_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                LEFT JOIN securities s
                  ON s.security_id = d.security_id
                WHERE d.security_id IS NOT NULL
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_events", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="orphan_delisting_event_listing_status_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                LEFT JOIN listing_status_intervals l
                  ON l.listing_status_id = d.source_listing_status_id
                WHERE l.listing_status_id IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_events", "listing_status_intervals"),
        ),
        SqlQualityCheck(
            dataset_id="delisting_events",
            table_name="delisting_events",
            check_name="orphan_delisting_event_return_observations",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_events d
                LEFT JOIN delisting_return_observations o
                  ON o.delisting_return_observation_id = d.return_observation_id
                WHERE d.return_observation_id IS NOT NULL
                  AND o.delisting_return_observation_id IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_events", "delisting_return_observations"),
        ),
        SqlQualityCheck(
            dataset_id="delisting_return_observations",
            table_name="delisting_return_observations",
            check_name="duplicate_delisting_return_observations",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT delisting_return_observation_id, count(*) AS row_count
                    FROM delisting_return_observations
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("delisting_return_observations",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_return_observations",
            table_name="delisting_return_observations",
            check_name="bad_delisting_return_observation_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_return_observations o
                WHERE o.delisting_return_observation_id IS NULL
                   OR o.delisting_return_observation_id = ''
                   OR o.source IS NULL
                   OR o.source = ''
                   OR o.provider IS NULL
                   OR o.provider = ''
                   OR (
                       coalesce(o.security_id, '') = ''
                       AND coalesce(o.symbol, '') = ''
                       AND coalesce(o.vendor_security_id, '') = ''
                   )
                   OR o.delist_date IS NULL
                   OR o.as_of_date IS NULL
                   OR o.available_at IS NULL
                   OR o.delisting_return IS NULL
                   OR o.delisting_return < -1.0
                   OR o.delisting_return > 10.0
                   OR o.return_basis IS NULL
                   OR o.return_basis = ''
            """,
            threshold=0.0,
            required_tables=("delisting_return_observations",),
        ),
        SqlQualityCheck(
            dataset_id="delisting_return_observations",
            table_name="delisting_return_observations",
            check_name="orphan_delisting_return_observation_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM delisting_return_observations o
                LEFT JOIN securities s
                  ON s.security_id = o.security_id
                WHERE o.security_id IS NOT NULL
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("delisting_return_observations", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_company_facts",
            table_name="fundamental_points",
            check_name="fundamental_period_after_asof",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_points
                WHERE period_end IS NOT NULL
                  AND period_end > as_of_date
            """,
            threshold=0.0,
            required_tables=("fundamental_points",),
        ),
        SqlQualityCheck(
            dataset_id="sec_company_facts",
            table_name="fundamental_points",
            check_name="missing_fundamental_available_at",
            sql="SELECT count(*)::DOUBLE FROM fundamental_points WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("fundamental_points",),
        ),
        SqlQualityCheck(
            dataset_id="sec_company_facts",
            table_name="fundamental_points",
            check_name="duplicate_fundamental_points",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        security_id,
                        metric,
                        coalesce(unit, '') AS unit,
                        period_start,
                        period_end,
                        as_of_date,
                        coalesce(accession_number, '') AS accession_number,
                        count(*) AS row_count
                    FROM fundamental_points
                    GROUP BY 1, 2, 3, 4, 5, 6, 7
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_points",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_concept_catalog",
            table_name="xbrl_concept_catalog",
            check_name="duplicate_xbrl_concept_catalog_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, taxonomy, concept, count(*) AS row_count
                    FROM xbrl_concept_catalog
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_concept_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_concept_catalog",
            table_name="xbrl_concept_catalog",
            check_name="bad_xbrl_concept_catalog_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_concept_catalog
                WHERE source IS NULL
                   OR source = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR units_json IS NULL
                   OR units_json IN ('', '[]')
                   OR forms_json IS NULL
                   OR forms_json IN ('', '[]')
                   OR fiscal_periods_json IS NULL
                   OR statement_category NOT IN ('balance_sheet', 'income_statement', 'cash_flow', 'per_share', 'share_count', 'bank_statement', 'insurance_statement', 'reit_statement', 'utility_statement', 'broker_dealer_statement', 'other')
                   OR fact_count <= 0
                   OR security_count <= 0
                   OR accession_count <= 0
                   OR (first_period_end IS NOT NULL AND last_period_end < first_period_end)
                   OR (first_filed_date IS NOT NULL AND last_filed_date < first_filed_date)
                   OR (first_available_at IS NOT NULL AND last_available_at < first_available_at)
            """,
            threshold=0.0,
            required_tables=("xbrl_concept_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_concept_catalog",
            table_name="xbrl_concept_catalog",
            check_name="sec_company_fact_concepts_without_catalog",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT source, taxonomy, concept
                    FROM sec_company_facts
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND concept IS NOT NULL
                      AND concept <> ''
                ) f
                LEFT JOIN xbrl_concept_catalog c
                  ON c.source = f.source
                 AND c.taxonomy = f.taxonomy
                 AND c.concept = f.concept
                WHERE c.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("sec_company_facts", "xbrl_concept_catalog"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_packages",
            check_name="duplicate_xbrl_taxonomy_packages",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT taxonomy_package_id, count(*) AS row_count
                    FROM xbrl_taxonomy_packages
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_taxonomy_packages",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_packages",
            check_name="bad_xbrl_taxonomy_packages",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_taxonomy_packages
                WHERE taxonomy_package_id IS NULL
                   OR taxonomy_package_id = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR release_year < 2000
                   OR source_url IS NULL
                   OR source_url = ''
                   OR package_sha256 IS NULL
                   OR length(package_sha256) <> 64
                   OR byte_count <= 0
                   OR file_count <= 0
                   OR linkbase_file_count <= 0
                   OR relationship_count <= 0
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_taxonomy_packages",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_relationships",
            check_name="duplicate_xbrl_taxonomy_relationships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT relationship_id, count(*) AS row_count
                    FROM xbrl_taxonomy_relationships
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_taxonomy_relationships",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_relationships",
            check_name="bad_xbrl_taxonomy_relationships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_taxonomy_relationships
                WHERE relationship_id IS NULL
                   OR relationship_id = ''
                   OR taxonomy_package_id IS NULL
                   OR taxonomy_package_id = ''
                   OR linkbase_type NOT IN ('presentation', 'calculation', 'definition')
                   OR source_file IS NULL
                   OR source_file = ''
                   OR parent_concept IS NULL
                   OR parent_concept = ''
                   OR child_concept IS NULL
                   OR child_concept = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
                   OR (linkbase_type = 'calculation' AND weight IS NULL)
            """,
            threshold=0.0,
            required_tables=("xbrl_taxonomy_relationships",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_taxonomy",
            table_name="xbrl_taxonomy_relationships",
            check_name="observed_xbrl_concepts_without_taxonomy_relationships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_concept_catalog c
                LEFT JOIN xbrl_taxonomy_relationships r
                  ON (
                        r.parent_taxonomy = c.taxonomy
                    AND r.parent_concept = c.concept
                  )
                  OR (
                        r.child_taxonomy = c.taxonomy
                    AND r.child_concept = c.concept
                  )
                WHERE c.taxonomy IN ('us-gaap', 'srt')
                  AND r.relationship_id IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_concept_catalog", "xbrl_taxonomy_relationships"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_dimensions",
            table_name="xbrl_dimension_edges",
            check_name="duplicate_xbrl_dimension_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT dimension_edge_id, count(*) AS row_count
                    FROM xbrl_dimension_edges
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_dimension_edges",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_dimensions",
            table_name="xbrl_dimension_edges",
            check_name="bad_xbrl_dimension_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_dimension_edges
                WHERE dimension_edge_id IS NULL
                   OR dimension_edge_id = ''
                   OR relationship_id IS NULL
                   OR relationship_id = ''
                   OR taxonomy_package_id IS NULL
                   OR taxonomy_package_id = ''
                   OR relationship_kind IS NULL
                   OR relationship_kind = ''
                   OR source_concept IS NULL
                   OR source_concept = ''
                   OR target_concept IS NULL
                   OR target_concept = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_dimension_edges",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_dimensions",
            table_name="xbrl_dimension_edges",
            check_name="dimension_edges_without_relationship",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_dimension_edges d
                LEFT JOIN xbrl_taxonomy_relationships r
                  ON r.relationship_id = d.relationship_id
                WHERE r.relationship_id IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_dimension_edges", "xbrl_taxonomy_relationships"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_fact_frames",
            table_name="xbrl_fact_frames",
            check_name="duplicate_xbrl_fact_frames",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, taxonomy, concept, unit, frame, count(*) AS row_count
                    FROM xbrl_fact_frames
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_fact_frames",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_fact_frames",
            table_name="xbrl_fact_frames",
            check_name="bad_xbrl_fact_frames",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_fact_frames
                WHERE fact_frame_id IS NULL
                   OR fact_frame_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR frame IS NULL
                   OR frame = ''
                   OR frame_period IS NULL
                   OR frame_period = ''
                   OR fact_count <= 0
                   OR security_count <= 0
                   OR accession_count <= 0
                   OR (frame_quarter IS NOT NULL AND (frame_quarter < 1 OR frame_quarter > 4))
                   OR (first_period_start IS NOT NULL AND last_period_end < first_period_start)
                   OR (first_filed_date IS NOT NULL AND last_filed_date < first_filed_date)
                   OR (first_available_at IS NOT NULL AND last_available_at < first_available_at)
            """,
            threshold=0.0,
            required_tables=("xbrl_fact_frames",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_fact_frames",
            table_name="xbrl_fact_frames",
            check_name="xbrl_fact_frames_without_sec_company_facts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_fact_frames f
                LEFT JOIN sec_company_facts s
                  ON s.source = f.source
                 AND s.taxonomy = f.taxonomy
                 AND s.concept = f.concept
                 AND s.unit = f.unit
                 AND s.frame = f.frame
                WHERE s.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_fact_frames", "sec_company_facts"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_contexts",
            check_name="duplicate_xbrl_filing_contexts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, accession_number, primary_document, context_id, count(*) AS row_count
                    FROM xbrl_filing_contexts
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_contexts",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_contexts",
            check_name="bad_xbrl_filing_context_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_contexts
                WHERE filing_context_id IS NULL
                   OR filing_context_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR primary_document IS NULL
                   OR primary_document = ''
                   OR context_id IS NULL
                   OR context_id = ''
                   OR period_type NOT IN ('instant', 'duration', 'forever', 'unknown')
                   OR (period_type = 'instant' AND instant_date IS NULL)
                   OR (period_type = 'duration' AND (period_start IS NULL OR period_end IS NULL))
                   OR explicit_member_count < 0
                   OR typed_member_count < 0
                   OR dimension_count <> explicit_member_count + typed_member_count
                   OR context_hash IS NULL
                   OR context_hash = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_contexts",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_contexts",
            check_name="xbrl_filing_contexts_without_sec_submission",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_contexts c
                LEFT JOIN sec_submissions s
                  ON s.security_id = c.security_id
                 AND s.accession_number = c.accession_number
                 AND s.primary_document = c.primary_document
                WHERE s.accession_number IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_contexts", "sec_submissions"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_dimensions",
            check_name="duplicate_xbrl_filing_dimensions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT filing_dimension_id, count(*) AS row_count
                    FROM xbrl_filing_dimensions
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_dimensions",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_dimensions",
            check_name="bad_xbrl_filing_dimension_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_dimensions
                WHERE filing_dimension_id IS NULL
                   OR filing_dimension_id = ''
                   OR filing_context_id IS NULL
                   OR filing_context_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR primary_document IS NULL
                   OR primary_document = ''
                   OR context_id IS NULL
                   OR context_id = ''
                   OR context_element NOT IN ('segment', 'scenario', 'unknown')
                   OR member_kind NOT IN ('explicit', 'typed')
                   OR dimension_qname IS NULL
                   OR dimension_qname = ''
                   OR dimension_concept IS NULL
                   OR dimension_concept = ''
                   OR (member_kind = 'explicit' AND (member_qname IS NULL OR member_qname = '' OR member_concept IS NULL OR member_concept = ''))
                   OR (member_kind = 'typed' AND (typed_member_value IS NULL OR typed_member_value = ''))
                   OR ordinal < 1
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_dimensions",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_contexts",
            table_name="xbrl_filing_dimensions",
            check_name="xbrl_filing_dimensions_without_context",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_dimensions d
                LEFT JOIN xbrl_filing_contexts c
                  ON c.filing_context_id = d.filing_context_id
                WHERE c.filing_context_id IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_dimensions", "xbrl_filing_contexts"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_facts",
            table_name="xbrl_filing_facts",
            check_name="duplicate_xbrl_filing_facts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT filing_fact_id, count(*) AS row_count
                    FROM xbrl_filing_facts
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_facts",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_facts",
            table_name="xbrl_filing_facts",
            check_name="bad_xbrl_filing_fact_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_facts
                WHERE filing_fact_id IS NULL
                   OR filing_fact_id = ''
                   OR filing_context_id IS NULL
                   OR filing_context_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR primary_document IS NULL
                   OR primary_document = ''
                   OR fact_ordinal < 1
                   OR fact_kind NOT IN ('nonFraction', 'nonNumeric', 'fraction')
                   OR qname IS NULL
                   OR qname = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR context_ref IS NULL
                   OR context_ref = ''
                   OR unit_measures_json IS NULL
                   OR unit_numerator_measures_json IS NULL
                   OR unit_denominator_measures_json IS NULL
                   OR is_numeric <> (fact_kind IN ('nonFraction', 'fraction'))
                   OR (is_numeric AND raw_value IS NOT NULL AND raw_value <> '' AND numeric_value IS NULL AND fact_kind = 'nonFraction' AND regexp_matches(replace(replace(raw_value, ',', ''), '$', ''), '^[[:space:]]*[-+]?[0-9]+(\\.[0-9]+)?[[:space:]]*$'))
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_facts",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_facts",
            table_name="xbrl_filing_facts",
            check_name="xbrl_filing_facts_without_context",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_facts f
                LEFT JOIN xbrl_filing_contexts c
                  ON c.filing_context_id = f.filing_context_id
                 AND c.context_id = f.context_ref
                WHERE c.filing_context_id IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_facts", "xbrl_filing_contexts"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_filing_facts",
            table_name="xbrl_filing_facts",
            check_name="xbrl_filing_facts_without_sec_submission",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_filing_facts f
                LEFT JOIN sec_submissions s
                  ON s.security_id = f.security_id
                 AND s.accession_number = f.accession_number
                 AND s.primary_document = f.primary_document
                WHERE s.accession_number IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_filing_facts", "sec_submissions"),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_validation",
            table_name="xbrl_validation_results",
            check_name="duplicate_xbrl_validation_results",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT validation_id, count(*) AS row_count
                    FROM xbrl_validation_results
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("xbrl_validation_results",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_validation",
            table_name="xbrl_validation_results",
            check_name="bad_xbrl_validation_result_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_validation_results
                WHERE validation_id IS NULL
                   OR validation_id = ''
                   OR validation_run_id IS NULL
                   OR validation_run_id = ''
                   OR rule_family NOT IN ('calculation_linkbase', 'dqc')
                   OR rule_code IS NULL
                   OR rule_code = ''
                   OR severity NOT IN ('info', 'warning', 'error')
                   OR status NOT IN ('passed', 'failed', 'warning', 'skipped')
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR primary_document IS NULL
                   OR primary_document = ''
                   OR parent_concept IS NULL
                   OR parent_concept = ''
                   OR context_ref IS NULL
                   OR context_ref = ''
                   OR tolerance < 0
                   OR child_count < 0
                   OR child_facts_json IS NULL
                   OR child_facts_json = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR source_loaded_at IS NULL
                   OR resolution_status IS NULL
                   OR resolution_status NOT IN (
                        'resolved_ok',
                        'resolved_dimensional_artifact',
                        'genuine_error',
                        'unresolved'
                   )
                   OR (rule_family = 'calculation_linkbase' AND parent_value IS NULL)
                   OR (rule_family = 'calculation_linkbase' AND child_weighted_sum IS NULL)
                   OR (rule_family = 'calculation_linkbase' AND absolute_difference IS NULL)
                   OR (
                        status = 'passed'
                        AND absolute_difference > tolerance
                        AND resolution_status <> 'resolved_dimensional_artifact'
                   )
                   OR (status = 'failed' AND absolute_difference <= tolerance)
                   OR (
                        rule_family = 'calculation_linkbase'
                        AND status = 'passed'
                        AND resolution_status NOT IN ('resolved_ok', 'resolved_dimensional_artifact')
                   )
                   OR (
                        rule_family = 'calculation_linkbase'
                        AND status = 'failed'
                        AND resolution_status <> 'genuine_error'
                   )
                   OR (
                        rule_family = 'dqc'
                        AND status = 'failed'
                        AND resolution_status <> 'genuine_error'
                   )
            """,
            threshold=0.0,
            required_tables=("xbrl_validation_results",),
        ),
        SqlQualityCheck(
            dataset_id="xbrl_validation",
            table_name="xbrl_validation_results",
            check_name="failed_xbrl_calculation_linkbase_checks",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_validation_results
                WHERE rule_family = 'calculation_linkbase'
                  AND resolution_status = 'genuine_error'
            """,
            threshold=0.0,
            required_tables=("xbrl_validation_results",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="duplicate_fundamental_fact_revision_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        taxonomy,
                        concept,
                        unit,
                        period_start,
                        period_end,
                        accession_number,
                        count(*) AS row_count
                    FROM fundamental_fact_revisions
                    GROUP BY 1, 2, 3, 4, 5, 6, 7, 8
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_fact_revisions",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="bad_fundamental_fact_revision_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_fact_revisions
                WHERE fact_revision_id IS NULL
                   OR fact_revision_id = ''
                   OR revision_group_id IS NULL
                   OR revision_group_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR period_end IS NULL
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR filed_date IS NULL
                   OR source_url IS NULL
                   OR source_url = ''
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR (revision_sequence = 1 AND previous_accession_number IS NOT NULL)
                   OR (revision_sequence > 1 AND previous_accession_number IS NULL)
                   OR (revision_sequence = 1 AND is_value_changed)
                   OR (first_filed_date IS NOT NULL AND latest_filed_date < first_filed_date)
                   OR (first_available_at IS NOT NULL AND latest_available_at < first_available_at)
            """,
            threshold=0.0,
            required_tables=("fundamental_fact_revisions",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="sec_company_facts_without_revision_rows",
            sql="""
                WITH fact_keys AS (
                    SELECT
                        sha256(
                            concat_ws(
                                '|',
                                source,
                                security_id,
                                taxonomy,
                                concept,
                                unit,
                                coalesce(CAST(period_start AS VARCHAR), ''),
                                CAST(period_end AS VARCHAR),
                                accession_number
                            )
                        ) AS fact_revision_id
                    FROM sec_company_facts
                )
                SELECT count(*)::DOUBLE
                FROM fact_keys f
                LEFT JOIN fundamental_fact_revisions r
                  ON r.fact_revision_id = f.fact_revision_id
                WHERE r.fact_revision_id IS NULL
            """,
            threshold=0.0,
            required_tables=("sec_company_facts", "fundamental_fact_revisions"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="revision_rows_without_sec_company_facts",
            sql="""
                WITH fact_keys AS (
                    SELECT
                        sha256(
                            concat_ws(
                                '|',
                                source,
                                security_id,
                                taxonomy,
                                concept,
                                unit,
                                coalesce(CAST(period_start AS VARCHAR), ''),
                                CAST(period_end AS VARCHAR),
                                accession_number
                            )
                        ) AS fact_revision_id
                    FROM sec_company_facts
                )
                SELECT count(*)::DOUBLE
                FROM fundamental_fact_revisions r
                LEFT JOIN fact_keys f
                  ON f.fact_revision_id = r.fact_revision_id
                WHERE f.fact_revision_id IS NULL
            """,
            threshold=0.0,
            required_tables=("sec_company_facts", "fundamental_fact_revisions"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="bad_latest_fundamental_fact_revisions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        revision_group_id,
                        sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows
                    FROM fundamental_fact_revisions
                    GROUP BY 1
                    HAVING latest_rows <> 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_fact_revisions",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_fact_revisions",
            table_name="fundamental_fact_revisions",
            check_name="bad_fundamental_revision_change_flags",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_fact_revisions
                WHERE is_value_changed <> CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE (value IS DISTINCT FROM previous_value)
                END
            """,
            threshold=0.0,
            required_tables=("fundamental_fact_revisions",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="duplicate_fundamental_statement_map_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, taxonomy, concept, industry_template, count(*) AS row_count
                    FROM fundamental_statement_map
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_map",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="bad_fundamental_statement_map_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_map
                WHERE source IS NULL
                   OR source = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR statement_type NOT IN ('balance_sheet', 'income_statement', 'cash_flow', 'per_share', 'share_count', 'bank_statement', 'insurance_statement', 'reit_statement', 'utility_statement', 'broker_dealer_statement', 'other')
                   OR statement_section IS NULL
                   OR statement_section = ''
                   OR canonical_metric IS NULL
                   OR canonical_metric = ''
                   OR canonical_label IS NULL
                   OR canonical_label = ''
                   OR period_type NOT IN ('instant', 'duration')
                   OR normal_balance NOT IN ('debit', 'credit', 'not_applicable')
                   OR unit_type NOT IN ('monetary', 'shares', 'per_share', 'ratio', 'count', 'quantity', 'other')
                   OR value_multiplier IS NULL
                   OR concept_priority < 1
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_map",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="loaded_xbrl_concepts_without_statement_map",
            sql="""
                SELECT count(*)::DOUBLE
                FROM xbrl_concept_catalog c
                LEFT JOIN fundamental_statement_map m
                  ON m.source = c.source
                 AND m.taxonomy = c.taxonomy
                 AND m.concept = c.concept
                 AND m.is_active
                WHERE m.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("xbrl_concept_catalog", "fundamental_statement_map"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_concept_coverage_report",
            table_name="fundamental_unmapped_concept_report",
            check_name="fundamental_unmapped_concept_report_empty",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_unmapped_concept_report
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_unmapped_concept_report",),
            detail_sql="""
                SELECT *
                FROM fundamental_unmapped_concept_report
                ORDER BY gap_type, coalesce(item_id, 999999999), taxonomy, concept
                LIMIT 25
            """,
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="duplicate_fundamental_statement_point_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT statement_point_id, count(*) AS row_count
                    FROM fundamental_statement_points
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="bad_fundamental_statement_point_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points
                WHERE statement_point_id IS NULL
                   OR statement_point_id = ''
                   OR fact_revision_id IS NULL
                   OR fact_revision_id = ''
                   OR revision_group_id IS NULL
                   OR revision_group_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR statement_type NOT IN ('balance_sheet', 'income_statement', 'cash_flow', 'per_share', 'share_count', 'bank_statement', 'insurance_statement', 'reit_statement', 'utility_statement', 'broker_dealer_statement', 'other')
                   OR statement_section IS NULL
                   OR statement_section = ''
                   OR canonical_metric IS NULL
                   OR canonical_metric = ''
                   OR canonical_label IS NULL
                   OR canonical_label = ''
                   OR taxonomy IS NULL
                   OR taxonomy = ''
                   OR concept IS NULL
                   OR concept = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR unit_type NOT IN ('monetary', 'shares', 'per_share', 'ratio', 'count', 'other')
                   OR period_type NOT IN ('instant', 'duration')
                   OR normal_balance NOT IN ('debit', 'credit', 'not_applicable')
                   OR period_end IS NULL
                   OR as_of_date IS NULL
                   OR period_end > as_of_date
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR source_url IS NULL
                   OR source_url = ''
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR (period_type = 'duration' AND period_start IS NULL)
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="statement_points_without_map",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points p
                LEFT JOIN fundamental_statement_map m
                  ON m.source = p.source
                 AND m.taxonomy = p.taxonomy
                 AND m.concept = p.concept
                 AND m.is_active
                WHERE m.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points", "fundamental_statement_map"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="statement_points_without_revision_row",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points p
                LEFT JOIN fundamental_fact_revisions r
                  ON r.fact_revision_id = p.fact_revision_id
                WHERE r.fact_revision_id IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points", "fundamental_fact_revisions"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="mapped_statement_concepts_without_points",
            sql="""
                WITH mapped_concepts AS (
                    SELECT DISTINCT m.source, m.taxonomy, m.concept
                    FROM fundamental_statement_map m
                    JOIN fundamental_fact_revisions r
                      ON r.source = m.source
                     AND r.taxonomy = m.taxonomy
                     AND r.concept = m.concept
                    WHERE m.is_active
                )
                SELECT count(*)::DOUBLE
                FROM mapped_concepts m
                LEFT JOIN fundamental_statement_points p
                  ON p.source = m.source
                 AND p.taxonomy = m.taxonomy
                 AND p.concept = m.concept
                WHERE p.concept IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_map", "fundamental_fact_revisions", "fundamental_statement_points"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="bad_latest_fundamental_statement_points",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        canonical_metric,
                        unit,
                        period_start,
                        period_end,
                        sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows
                    FROM fundamental_statement_points
                    GROUP BY 1, 2, 3, 4, 5, 6
                    HAVING latest_rows <> 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_points",
            table_name="fundamental_statement_points",
            check_name="bad_fundamental_statement_value_mapping",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points p
                JOIN fundamental_statement_map m
                  ON m.source = p.source
                 AND m.taxonomy = p.taxonomy
                 AND m.concept = p.concept
                WHERE p.value IS DISTINCT FROM p.raw_value * m.value_multiplier
                   OR p.previous_value IS DISTINCT FROM p.previous_raw_value * m.value_multiplier
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points", "fundamental_statement_map"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_item_alias",
            table_name="fundamental_item_alias",
            check_name="duplicate_fundamental_item_alias_item_mappings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT alias_scheme, alias_code
                    FROM fundamental_item_alias
                    WHERE alias_scheme IS NOT NULL
                      AND alias_scheme <> ''
                      AND alias_code IS NOT NULL
                      AND alias_code <> ''
                    GROUP BY 1, 2
                    HAVING count(DISTINCT item_id) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_item_alias",),
            detail_sql="""
                SELECT
                    alias_scheme AS taxonomy,
                    alias_code AS concept,
                    count(DISTINCT item_id) AS item_id_count,
                    min(item_id) AS min_item_id,
                    max(item_id) AS max_item_id
                FROM fundamental_item_alias
                WHERE alias_scheme IS NOT NULL
                  AND alias_scheme <> ''
                  AND alias_code IS NOT NULL
                  AND alias_code <> ''
                GROUP BY 1, 2
                HAVING count(DISTINCT item_id) > 1
                ORDER BY taxonomy, concept
                LIMIT 50
            """,
        ),
        SqlQualityCheck(
            dataset_id="fundamental_item_alias",
            table_name="fundamental_item_alias",
            check_name="unmapped_fundamental_fact_concepts",
            sql="""
                WITH fact_concepts AS (
                    SELECT 'fundamental_statement_points' AS fact_table, taxonomy, concept
                    FROM fundamental_statement_points
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND concept IS NOT NULL
                      AND concept <> ''
                    UNION ALL
                    SELECT 'fundamental_points' AS fact_table, taxonomy, metric AS concept
                    FROM fundamental_points
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND metric IS NOT NULL
                      AND metric <> ''
                ),
                alias_counts AS (
                    SELECT alias_scheme, alias_code, count(DISTINCT item_id) AS item_id_count
                    FROM fundamental_item_alias
                    WHERE alias_scheme IS NOT NULL
                      AND alias_scheme <> ''
                      AND alias_code IS NOT NULL
                      AND alias_code <> ''
                    GROUP BY 1, 2
                )
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT f.fact_table, f.taxonomy, f.concept
                    FROM fact_concepts f
                    LEFT JOIN alias_counts a
                      ON a.alias_scheme = f.taxonomy
                     AND a.alias_code = f.concept
                    WHERE coalesce(a.item_id_count, 0) = 0
                )
            """,
            threshold=0.0,
            required_tables=(
                "fundamental_statement_points",
                "fundamental_points",
                "fundamental_item_alias",
            ),
            failure_status="warning",
            detail_sql="""
                WITH fact_concepts AS (
                    SELECT 'fundamental_statement_points' AS fact_table, taxonomy, concept
                    FROM fundamental_statement_points
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND concept IS NOT NULL
                      AND concept <> ''
                    UNION ALL
                    SELECT 'fundamental_points' AS fact_table, taxonomy, metric AS concept
                    FROM fundamental_points
                    WHERE taxonomy IS NOT NULL
                      AND taxonomy <> ''
                      AND metric IS NOT NULL
                      AND metric <> ''
                ),
                alias_counts AS (
                    SELECT alias_scheme, alias_code, count(DISTINCT item_id) AS item_id_count
                    FROM fundamental_item_alias
                    WHERE alias_scheme IS NOT NULL
                      AND alias_scheme <> ''
                      AND alias_code IS NOT NULL
                      AND alias_code <> ''
                    GROUP BY 1, 2
                )
                SELECT DISTINCT f.fact_table, f.taxonomy, f.concept
                FROM fact_concepts f
                LEFT JOIN alias_counts a
                  ON a.alias_scheme = f.taxonomy
                 AND a.alias_code = f.concept
                WHERE coalesce(a.item_id_count, 0) = 0
                ORDER BY f.fact_table, f.taxonomy, f.concept
                LIMIT 50
            """,
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="duplicate_fundamental_ttm_point_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT ttm_point_id, count(*) AS row_count
                    FROM fundamental_ttm_points
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="bad_fundamental_ttm_point_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ttm_points
                WHERE ttm_point_id IS NULL
                   OR ttm_point_id = ''
                   OR ttm_revision_group_id IS NULL
                   OR ttm_revision_group_id = ''
                   OR anchor_statement_point_id IS NULL
                   OR anchor_statement_point_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR statement_type NOT IN ('income_statement', 'cash_flow', 'per_share')
                   OR statement_section IS NULL
                   OR statement_section = ''
                   OR canonical_metric IS NULL
                   OR canonical_metric = ''
                   OR canonical_label IS NULL
                   OR canonical_label = ''
                   OR unit IS NULL
                   OR unit = ''
                   OR unit_type NOT IN ('monetary', 'per_share')
                   OR ttm_start_date IS NULL
                   OR ttm_end_date IS NULL
                   OR ttm_start_date > ttm_end_date
                   OR as_of_date IS NULL
                   OR ttm_end_date > as_of_date
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR quarter_count <> 4
                   OR coverage_days NOT BETWEEN 330 AND 380
                   OR min_input_available_at IS NULL
                   OR max_input_available_at IS NULL
                   OR max_input_available_at < min_input_available_at
                   OR (available_at IS NOT NULL AND available_at < max_input_available_at)
                   OR input_statement_point_ids_json IS NULL
                   OR input_statement_point_ids_json IN ('', '[]')
                   OR input_accessions_json IS NULL
                   OR input_accessions_json IN ('', '[]')
                   OR input_period_ends_json IS NULL
                   OR input_period_ends_json IN ('', '[]')
                   OR ttm_value IS NULL
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR (revision_sequence = 1 AND is_value_changed)
                   OR calculation_method NOT IN (
                       'sum_four_visible_quarter_like_statement_points_with_ytd_quarter_derivations',
                       'stitched_quarterly_ttm'
                   )
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="ttm_points_without_anchor_statement_point",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ttm_points t
                LEFT JOIN fundamental_statement_points p
                  ON p.statement_point_id = t.anchor_statement_point_id
                WHERE p.statement_point_id IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points", "fundamental_statement_points"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="bad_latest_fundamental_ttm_points",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        ttm_revision_group_id,
                        sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows
                    FROM fundamental_ttm_points
                    GROUP BY 1
                    HAVING latest_rows <> 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ttm_points",
            table_name="fundamental_ttm_points",
            check_name="bad_fundamental_ttm_change_flags",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ttm_points
                WHERE is_value_changed <> CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE (ttm_value IS DISTINCT FROM previous_ttm_value)
                END
            """,
            threshold=0.0,
            required_tables=("fundamental_ttm_points",),
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="fundamental_calendar_map",
            check_name="calendarization_map_exactly_one_label",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        p.fundamental_period_id,
                        count(m.calendar_map_id) AS map_rows,
                        sum(CASE
                            WHEN m.fiscal_scheme_period IS NULL
                              OR m.fiscal_scheme_period = ''
                              OR m.containing_calendar_period IS NULL
                              OR m.containing_calendar_period = ''
                              OR m.greatest_overlap_calendar_period IS NULL
                              OR m.greatest_overlap_calendar_period = ''
                            THEN 1 ELSE 0
                        END) AS bad_label_rows
                    FROM fundamental_periods p
                    LEFT JOIN fundamental_calendar_map m
                      ON m.fundamental_period_id = p.fundamental_period_id
                     AND m.is_latest_revision
                    GROUP BY p.fundamental_period_id
                    HAVING map_rows <> 1
                        OR bad_label_rows <> 0
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_periods", "fundamental_calendar_map"),
            detail_sql="""
                SELECT
                    p.fundamental_period_id,
                    count(m.calendar_map_id) AS map_rows,
                    sum(CASE
                        WHEN m.fiscal_scheme_period IS NULL
                          OR m.fiscal_scheme_period = ''
                          OR m.containing_calendar_period IS NULL
                          OR m.containing_calendar_period = ''
                          OR m.greatest_overlap_calendar_period IS NULL
                          OR m.greatest_overlap_calendar_period = ''
                        THEN 1 ELSE 0
                    END) AS bad_label_rows
                FROM fundamental_periods p
                LEFT JOIN fundamental_calendar_map m
                  ON m.fundamental_period_id = p.fundamental_period_id
                 AND m.is_latest_revision
                GROUP BY p.fundamental_period_id
                HAVING map_rows <> 1
                    OR bad_label_rows <> 0
                ORDER BY p.fundamental_period_id
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="fundamental_calendar_map",
            check_name="calendarization_53_week_flagged",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_calendar_map
                WHERE (
                       period_length_days >= 371
                    OR (normalized_period_type = 'annual' AND period_length_days > 364)
                    OR (normalized_period_type = 'quarter' AND period_length_days > 91)
                )
                  AND NOT is_53_week
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_calendar_map",),
            detail_sql="""
                SELECT
                    fundamental_period_id,
                    security_id,
                    period_start,
                    period_end,
                    normalized_period_type,
                    period_length_days,
                    week_count,
                    is_53_week
                FROM fundamental_calendar_map
                WHERE (
                       period_length_days >= 371
                    OR (normalized_period_type = 'annual' AND period_length_days > 364)
                    OR (normalized_period_type = 'quarter' AND period_length_days > 91)
                )
                  AND NOT is_53_week
                ORDER BY security_id, period_end
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="fundamental_calendar_ttm",
            check_name="calendar_ttm_no_duplicate_windows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        upstream_source,
                        security_id,
                        calendar_period,
                        canonical_metric,
                        unit,
                        revision_sequence,
                        count(*) AS row_count
                    FROM fundamental_calendar_ttm
                    GROUP BY 1, 2, 3, 4, 5, 6, 7
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_calendar_ttm",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="fundamental_ttm_points",
            check_name="stitched_ttm_no_duplicate_windows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        ttm_end_date,
                        canonical_metric,
                        unit,
                        revision_sequence,
                        count(*) AS row_count
                    FROM fundamental_ttm_points
                    WHERE calculation_method = 'stitched_quarterly_ttm'
                    GROUP BY 1, 2, 3, 4, 5, 6
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ttm_points",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="calendarization",
            table_name="calendarization_coverage",
            check_name="calendarization_coverage_green",
            sql="""
                SELECT coalesce(sum(
                    fiscal_scheme_unmapped_count
                    + containing_scheme_unmapped_count
                    + overlap_scheme_unmapped_count
                    + duplicate_map_count
                    + unflagged_53_week_count
                    + duplicate_calendar_ttm_window_count
                    + duplicate_stitched_ttm_window_count
                ), 0)::DOUBLE
                FROM calendarization_coverage
                WHERE is_latest_revision
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("calendarization_coverage",),
            detail_sql="""
                SELECT *
                FROM calendarization_coverage
                WHERE is_latest_revision
                  AND (
                    fiscal_scheme_unmapped_count
                    + containing_scheme_unmapped_count
                    + overlap_scheme_unmapped_count
                    + duplicate_map_count
                    + unflagged_53_week_count
                    + duplicate_calendar_ttm_window_count
                    + duplicate_stitched_ttm_window_count
                  ) <> 0
                ORDER BY available_at DESC
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="segments",
            table_name="segment_fact",
            check_name="bad_segment_fact_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM segment_fact
                WHERE segment_fact_id IS NULL OR segment_fact_id = ''
                   OR segment_dim_id IS NULL OR segment_dim_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR canonical_item IS NULL OR canonical_item = ''
                   OR consolidated_code IS NULL OR consolidated_code = ''
                   OR value IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR source_loaded_at IS NULL
                   OR reconciliation_status NOT IN ('reconciled', 'flagged_divergent', 'no_consolidated')
                   OR reconciliation_tolerance IS NULL
                   OR revision_sequence < 1
                   OR revision_count < revision_sequence
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("segment_fact",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="segments",
            table_name="segment_fact",
            check_name="duplicate_segment_fact_revision_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        segment_dim_id,
                        canonical_item,
                        coalesce(CAST(period_start AS VARCHAR), ''),
                        as_of_date,
                        revision_sequence,
                        count(*) AS row_count
                    FROM segment_fact
                    GROUP BY 1, 2, 3, 4, 5, 6, 7
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("segment_fact",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="segments",
            table_name="segment_fact",
            check_name="segment_reconciliation_divergence_warning",
            sql="""
                SELECT count(*)::DOUBLE
                FROM segment_fact
                WHERE is_latest_revision
                  AND reconciliation_status = 'flagged_divergent'
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("segment_fact",),
            failure_status="warning",
            detail_sql="""
                SELECT
                    security_id,
                    canonical_item,
                    as_of_date,
                    segment_sum_value,
                    consolidated_value,
                    reconciliation_difference,
                    reconciliation_relative_difference,
                    reconciliation_tolerance
                FROM segment_fact
                WHERE is_latest_revision
                  AND reconciliation_status = 'flagged_divergent'
                ORDER BY abs(coalesce(reconciliation_relative_difference, 0)) DESC,
                         security_id,
                         canonical_item
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="footnotes",
            table_name="footnote_pension",
            check_name="bad_footnote_subledger_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT pension_id AS row_id, source, security_id, line_item, value, as_of_date, available_at, source_loaded_at, revision_sequence, revision_count
                    FROM footnote_pension
                    UNION ALL
                    SELECT deferred_tax_id AS row_id, source, security_id, line_item, value, as_of_date, available_at, source_loaded_at, revision_sequence, revision_count
                    FROM footnote_deferred_tax
                    UNION ALL
                    SELECT lease_id AS row_id, source, security_id, line_item, value, as_of_date, available_at, source_loaded_at, revision_sequence, revision_count
                    FROM footnote_lease
                    UNION ALL
                    SELECT sbc_id AS row_id, source, security_id, line_item, value, as_of_date, available_at, source_loaded_at, revision_sequence, revision_count
                    FROM footnote_sbc
                )
                WHERE row_id IS NULL OR row_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR line_item IS NULL OR line_item = ''
                   OR value IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR source_loaded_at IS NULL
                   OR revision_sequence < 1
                   OR revision_count < revision_sequence
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=(
                "footnote_pension",
                "footnote_deferred_tax",
                "footnote_lease",
                "footnote_sbc",
            ),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="segment_footnote_coverage",
            table_name="segment_footnote_coverage",
            check_name="segment_footnote_coverage_counts_valid",
            sql="""
                SELECT count(*)::DOUBLE
                FROM segment_footnote_coverage
                WHERE segment_dim_count < 0
                   OR segment_fact_count < 0
                   OR segment_reconciled_count < 0
                   OR segment_flagged_divergent_count < 0
                   OR segment_no_consolidated_count < 0
                   OR footnote_pension_count < 0
                   OR footnote_deferred_tax_count < 0
                   OR footnote_lease_count < 0
                   OR footnote_sbc_count < 0
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("segment_footnote_coverage",),
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_periods",
            table_name="fundamental_periods",
            check_name="duplicate_fundamental_period_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        security_id,
                        period_start,
                        period_end,
                        accession_number,
                        count(*) AS row_count
                    FROM fundamental_periods
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_periods",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_periods",
            table_name="fundamental_periods",
            check_name="bad_fundamental_period_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_periods
                WHERE fundamental_period_id IS NULL
                   OR fundamental_period_id = ''
                   OR period_group_id IS NULL
                   OR period_group_id = ''
                   OR source IS NULL
                   OR source = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR period_end IS NULL
                   OR datadate IS NULL
                   OR datadate <> period_end
                   OR as_of_date IS NULL
                   OR fdate IS NULL
                   OR fdate <> as_of_date
                   OR ldate IS NULL
                   OR ldate < fdate
                   OR (rdq IS NOT NULL AND rdq < datadate)
                   OR (rdq IS NOT NULL AND rdq > fdate)
                   OR (pdate IS NOT NULL AND pdate IS DISTINCT FROM rdq)
                   OR period_end > as_of_date
                   OR available_at IS NULL
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR normalized_period_type NOT IN (
                       'instant',
                       'quarter',
                       'semiannual_ytd',
                       'multi_quarter_ytd',
                       'annual',
                       'multi_year_comparative',
                       'other'
                   )
                   OR (period_start IS NOT NULL AND period_start > period_end)
                   OR (period_start IS NULL AND period_days IS NOT NULL)
                   OR (period_start IS NOT NULL AND period_days <> date_diff('day', period_start, period_end) + 1)
                   OR calendar_year IS NULL
                   OR calendar_quarter NOT BETWEEN 1 AND 4
                   OR calendar_period IS NULL
                   OR calendar_period = ''
                   OR reported_fiscal_years_json IS NULL
                   OR reported_fiscal_periods_json IS NULL
                   OR statement_types_json IS NULL
                   OR statement_types_json IN ('', '[]')
                   OR canonical_metrics_json IS NULL
                   OR canonical_metrics_json IN ('', '[]')
                   OR input_statement_point_ids_json IS NULL
                   OR input_statement_point_ids_json IN ('', '[]')
                   OR statement_point_count <= 0
                   OR canonical_metric_count <= 0
                   OR concept_count <= 0
                   OR value_changed_statement_count < 0
                   OR revision_sequence < 1
                   OR revision_count < 1
                   OR revision_sequence > revision_count
                   OR is_latest_revision <> (revision_sequence = revision_count)
                   OR (first_available_at IS NOT NULL AND latest_available_at < first_available_at)
            """,
            threshold=0.0,
            required_tables=("fundamental_periods",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_periods",
            table_name="fundamental_periods",
            check_name="statement_points_without_fundamental_period",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_statement_points p
                LEFT JOIN fundamental_periods fp
                  ON fp.source = p.source
                 AND fp.security_id = p.security_id
                 AND fp.period_end = p.period_end
                 AND fp.accession_number = p.accession_number
                 AND (
                     fp.period_start IS NOT DISTINCT FROM p.period_start
                 )
                WHERE fp.fundamental_period_id IS NULL
            """,
            threshold=0.0,
            required_tables=("fundamental_statement_points", "fundamental_periods"),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_periods",
            table_name="fundamental_periods",
            check_name="bad_latest_fundamental_periods",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        period_group_id,
                        sum(CASE WHEN is_latest_revision THEN 1 ELSE 0 END) AS latest_rows
                    FROM fundamental_periods
                    GROUP BY 1
                    HAVING latest_rows <> 1
                )
            """,
            threshold=0.0,
            required_tables=("fundamental_periods",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest",
            table_name="finra_short_interest",
            check_name="bad_finra_short_quantities",
            sql="""
                SELECT count(*)::DOUBLE
                FROM finra_short_interest
                WHERE current_short_position_quantity < 0
                   OR previous_short_position_quantity < 0
                   OR average_daily_volume_quantity < 0
                   OR days_to_cover_quantity < 0
            """,
            threshold=0.0,
            required_tables=("finra_short_interest",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest",
            table_name="finra_short_interest",
            check_name="duplicate_finra_short_interest",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        settlement_date,
                        symbol,
                        coalesce(market_class_code, '') AS market_class_code,
                        count(*) AS row_count
                    FROM finra_short_interest
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("finra_short_interest",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest_backfills",
            table_name="finra_short_interest_backfill_manifests",
            check_name="bad_finra_short_interest_backfill_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM finra_short_interest_backfill_manifests
                WHERE manifest_id IS NULL
                   OR manifest_id = ''
                   OR status NOT IN ('succeeded', 'skipped')
                   OR start_date IS NULL
                   OR end_date IS NULL
                   OR start_date > end_date
                   OR date_order NOT IN ('asc', 'desc')
                   OR limit_dates < 1
                   OR (skip_existing_min_symbols IS NOT NULL AND skip_existing_min_symbols < 1)
                   OR candidate_count < 0
                   OR selected_date_count < 0
                   OR loaded_date_count < 0
                   OR source_row_count < 0
                   OR (feature_row_count IS NOT NULL AND feature_row_count < 0)
                   OR selected_dates_json IS NULL
                   OR selected_dates_json = ''
                   OR candidates_json IS NULL
                   OR candidates_json = ''
                   OR load_results_json IS NULL
                   OR load_results_json = ''
                   OR source IS NULL
                   OR source = ''
                   OR started_at IS NULL
                   OR finished_at IS NULL
                   OR finished_at < started_at
            """,
            threshold=0.0,
            required_tables=("finra_short_interest_backfill_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest_backfills",
            table_name="finra_short_interest_backfill_manifests",
            check_name="inconsistent_finra_short_interest_backfill_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM finra_short_interest_backfill_manifests
                WHERE (status = 'succeeded' AND loaded_date_count <> selected_date_count)
                   OR (status = 'succeeded' AND selected_date_count = 0)
                   OR (status = 'skipped' AND loaded_date_count <> 0)
                   OR selected_date_count > candidate_count
            """,
            threshold=0.0,
            required_tables=("finra_short_interest_backfill_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest_features",
            table_name="feature_values",
            check_name="missing_finra_short_interest_feature_values",
            sql="""
                WITH expected AS (
                    SELECT DISTINCT security_id, settlement_date AS as_of_date
                    FROM finra_short_interest
                    WHERE security_id IS NOT NULL
                      AND security_id <> ''
                      AND settlement_date IS NOT NULL
                )
                SELECT count(*)::DOUBLE
                FROM expected e
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM feature_values f
                    WHERE f.feature_set = 'finra_short_interest_v1'
                      AND f.feature_name = 'si_current_short_position'
                      AND f.security_id = e.security_id
                      AND f.as_of_date = e.as_of_date
                )
            """,
            threshold=0.0,
            required_tables=("finra_short_interest", "feature_values"),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_interest_features",
            table_name="feature_values",
            check_name="missing_finra_short_interest_xsec_features",
            sql="""
                WITH panel AS (
                    SELECT
                        security_id,
                        settlement_date AS as_of_date,
                        sum(coalesce(current_short_position_quantity, 0))::DOUBLE AS current_short_position_quantity,
                        sum(coalesce(average_daily_volume_quantity, 0))::DOUBLE AS average_daily_volume_quantity
                    FROM finra_short_interest
                    WHERE security_id IS NOT NULL
                      AND security_id <> ''
                      AND settlement_date IS NOT NULL
                    GROUP BY 1, 2
                ),
                eligible AS (
                    SELECT
                        security_id,
                        as_of_date,
                        count(*) OVER (PARTITION BY as_of_date) AS eligible_security_count
                    FROM panel
                    WHERE average_daily_volume_quantity > 0
                      AND current_short_position_quantity IS NOT NULL
                )
                SELECT count(*)::DOUBLE
                FROM eligible e
                WHERE e.eligible_security_count >= 20
                  AND NOT EXISTS (
                      SELECT 1
                      FROM feature_values f
                      WHERE f.feature_set = 'finra_short_interest_v1'
                        AND f.feature_name = 'si_short_to_adv_xsec_percentile'
                        AND f.security_id = e.security_id
                        AND f.as_of_date = e.as_of_date
                  )
            """,
            threshold=0.0,
            required_tables=("finra_short_interest", "feature_values"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f",
            table_name="thirteenf_holdings",
            check_name="bad_13f_holding_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_holdings
                WHERE accession_number IS NULL
                   OR accession_number = ''
                   OR cusip IS NULL
                   OR cusip = ''
                   OR source_period IS NULL
                   OR source_period = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_holdings",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f",
            table_name="thirteenf_holdings",
            check_name="duplicate_13f_holdings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT accession_number, infotable_sk, source_period, count(*) AS row_count
                    FROM thirteenf_holdings
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_holdings",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f",
            table_name="thirteenf_holdings",
            check_name="missing_13f_security_ids",
            sql="SELECT count(*)::DOUBLE FROM thirteenf_holdings WHERE security_id IS NULL OR security_id = ''",
            threshold=0.0,
            required_tables=("thirteenf_holdings",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f",
            table_name="security_identifier_history",
            check_name="thirteenf_cusips_without_identifier_history",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT cusip
                    FROM thirteenf_holdings
                ) h
                LEFT JOIN security_identifier_history i
                  ON i.id_type = 'CUSIP'
                 AND i.id_value = h.cusip
                WHERE i.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("thirteenf_holdings", "security_identifier_history"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_managers",
            check_name="duplicate_13f_manager_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT manager_id, count(*) AS row_count
                    FROM thirteenf_managers
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_managers",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_manager_reports",
            check_name="duplicate_13f_manager_report_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT manager_report_id, count(*) AS row_count
                    FROM thirteenf_manager_reports
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_manager_reports",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_positions",
            check_name="duplicate_13f_position_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT position_id, count(*) AS row_count
                    FROM thirteenf_security_positions
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_positions",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_ownership",
            check_name="duplicate_13f_ownership_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT ownership_id, count(*) AS row_count
                    FROM thirteenf_security_ownership
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_ownership",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_managers",
            check_name="bad_13f_manager_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_managers
                WHERE manager_id IS NULL
                   OR manager_id = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR filing_count < 1
                   OR amendment_count < 0
                   OR source_period_count < 1
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_managers",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_manager_reports",
            check_name="bad_13f_manager_report_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_manager_reports
                WHERE manager_report_id IS NULL
                   OR manager_report_id = ''
                   OR manager_id IS NULL
                   OR manager_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR cik IS NULL
                   OR cik = ''
                   OR report_period IS NULL
                   OR filing_date IS NULL
                   OR report_period > filing_date
                   OR source_period IS NULL
                   OR source_period = ''
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_manager_reports",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_positions",
            check_name="bad_13f_security_position_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_positions
                WHERE position_id IS NULL
                   OR position_id = ''
                   OR manager_report_id IS NULL
                   OR manager_report_id = ''
                   OR manager_id IS NULL
                   OR manager_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR cusip IS NULL
                   OR cusip = ''
                   OR report_period IS NULL
                   OR filing_date IS NULL
                   OR report_period > filing_date
                   OR as_of_date <> report_period
                   OR source_period IS NULL
                   OR source_period = ''
                   OR coalesce(value_usd, 0) < 0
                   OR coalesce(share_quantity, 0) < 0
                   OR coalesce(portfolio_weight, 0) < 0
                   OR portfolio_weight > 1.000001
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_positions",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_ownership",
            check_name="bad_13f_security_ownership_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_ownership
                WHERE ownership_id IS NULL
                   OR ownership_id = ''
                   OR cusip IS NULL
                   OR cusip = ''
                   OR report_period IS NULL
                   OR as_of_date <> report_period
                   OR source_period IS NULL
                   OR source_period = ''
                   OR holding_row_count < 1
                   OR filing_count < 1
                   OR holder_count < 1
                   OR common_holder_count < 0
                   OR coalesce(common_value_usd, 0) < 0
                   OR coalesce(common_share_quantity, 0) < 0
                   OR coalesce(call_share_quantity, 0) < 0
                   OR coalesce(put_share_quantity, 0) < 0
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_ownership",),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_manager_reports",
            check_name="orphan_13f_manager_reports",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_manager_reports r
                LEFT JOIN thirteenf_managers m
                  ON m.manager_id = r.manager_id
                WHERE m.manager_id IS NULL
            """,
            threshold=0.0,
            required_tables=("thirteenf_manager_reports", "thirteenf_managers"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_positions",
            check_name="orphan_13f_security_positions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_positions p
                LEFT JOIN thirteenf_manager_reports r
                  ON r.manager_report_id = p.manager_report_id
                WHERE r.manager_report_id IS NULL
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_positions", "thirteenf_manager_reports"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="thirteenf_security_ownership",
            check_name="orphan_13f_ownership_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_ownership o
                LEFT JOIN securities s
                  ON s.security_id = o.security_id
                WHERE o.security_id IS NOT NULL
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_ownership", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="sec_13f_ownership_features",
            table_name="feature_values",
            check_name="missing_13f_ownership_feature_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_security_ownership o
                WHERE o.security_id IS NOT NULL
                  AND NOT EXISTS (
                      SELECT 1
                      FROM feature_values f
                      WHERE f.feature_set = 'sec_13f_ownership_v1'
                        AND f.feature_name = 'own_13f_holder_count'
                        AND f.security_id = o.security_id
                        AND f.as_of_date = o.as_of_date
                  )
            """,
            threshold=0.0,
            required_tables=("thirteenf_security_ownership", "feature_values"),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="filing_form4",
            check_name="duplicate_form4_filings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT accession_number, count(*) AS row_count
                    FROM filing_form4
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("filing_form4",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="filing_form4",
            check_name="bad_form4_filing_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM filing_form4
                WHERE filing_id IS NULL
                   OR filing_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR form_type NOT IN ('3', '3/A', '4', '4/A', '5', '5/A')
                   OR available_at IS NULL
                   OR source_url IS NULL
                   OR source_url = ''
            """,
            threshold=0.0,
            required_tables=("filing_form4",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="insider_relationship",
            check_name="bad_insider_relationship_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_relationship
                WHERE relationship_id IS NULL
                   OR relationship_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR insider_id IS NULL
                   OR insider_id = ''
                   OR full_name IS NULL
                   OR full_name = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR valid_from IS NULL
                   OR (valid_to IS NOT NULL AND valid_to <= valid_from)
                   OR available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("insider_relationship",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="insider_transaction",
            check_name="duplicate_insider_transaction_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT transaction_id, count(*) AS row_count
                    FROM insider_transaction
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("insider_transaction",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="insider_transaction",
            check_name="bad_insider_transaction_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_transaction
                WHERE transaction_id IS NULL
                   OR transaction_id = ''
                   OR filing_id IS NULL
                   OR filing_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR insider_id IS NULL
                   OR insider_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR transaction_date IS NULL
                   OR transaction_code IS NULL
                   OR upper(transaction_code) NOT IN (
                        'P','S','A','D','F','I','M','G','L','W','Z','C','E','H',
                        'O','X','U','J','K','V','Q','R','B','T','N','Y','1','2'
                   )
                   OR coalesce(transaction_shares, 0) < 0
                   OR coalesce(shares_owned_following, 0) < 0
                   OR available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("insider_transaction",),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="insider_transaction",
            check_name="orphan_insider_transactions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_transaction t
                LEFT JOIN filing_form4 f ON f.filing_id = t.filing_id
                LEFT JOIN insider i ON i.insider_id = t.insider_id
                WHERE f.filing_id IS NULL
                   OR i.insider_id IS NULL
            """,
            threshold=0.0,
            required_tables=("insider_transaction", "filing_form4", "insider"),
        ),
        SqlQualityCheck(
            dataset_id="sec_insider_ownership",
            table_name="tradingplan_10b5_1",
            check_name="bad_tradingplan_10b5_1_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM tradingplan_10b5_1
                WHERE plan_id IS NULL
                   OR plan_id = ''
                   OR insider_id IS NULL
                   OR insider_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR adoption_date IS NULL
                   OR transaction_count < 1
                   OR cooling_off_days < 0
                   OR available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("tradingplan_10b5_1",),
        ),
        SqlQualityCheck(
            dataset_id="insider_transaction_metrics",
            table_name="insider_transaction_metrics",
            check_name="insider_transaction_metrics_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, security_id, signal_date, window_days,
                           count(*) FILTER (WHERE is_latest_revision) AS latest_rows
                    FROM insider_transaction_metrics
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) FILTER (WHERE is_latest_revision) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("insider_transaction_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="insider_transaction_metrics",
            table_name="insider_transaction_metrics",
            check_name="bad_insider_transaction_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_transaction_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR signal_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR window_days < 1
                   OR cluster_min_buyers < 1
                   OR cluster_min_purchase_value < 0
                   OR transaction_count < 1
                   OR open_market_purchase_count < 0
                   OR open_market_sale_count < 0
                   OR discretionary_sale_count < 0
                   OR plan_sale_count < 0
                   OR unique_insider_count < 0
                   OR buyer_count < 0
                   OR seller_count < 0
                   OR cluster_purchase_count < 0
                   OR cluster_buyer_count < 0
                   OR cluster_sale_count < 0
                   OR cluster_seller_count < 0
                   OR coalesce(gross_purchase_shares, 0) < 0
                   OR coalesce(gross_sale_shares, 0) < 0
                   OR coalesce(gross_purchase_value, 0) < 0
                   OR coalesce(gross_sale_value, 0) < 0
                   OR coalesce(discretionary_sale_value, 0) < 0
                   OR coalesce(plan_sale_value, 0) < 0
                   OR coalesce(cluster_purchase_value, 0) < 0
                   OR coalesce(cluster_sale_value, 0) < 0
                   OR (plan_sale_value_ratio IS NOT NULL AND (plan_sale_value_ratio < 0 OR plan_sale_value_ratio > 1))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("insider_transaction_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="insider_transaction_metrics",
            table_name="insider_transaction_metrics",
            check_name="insider_transaction_metric_flags_consistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM insider_transaction_metrics
                WHERE (is_cluster_buy AND (
                           cluster_buyer_count < cluster_min_buyers
                           OR coalesce(cluster_purchase_value, 0) < cluster_min_purchase_value
                       ))
                   OR (is_10b5_1_heavy_sale AND (
                           coalesce(gross_sale_value, 0) <= 0
                           OR coalesce(plan_sale_value_ratio, 0) < 0.5
                       ))
                   OR plan_sale_value > gross_sale_value
                   OR discretionary_sale_value > gross_sale_value
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("insider_transaction_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="security_listing_metrics",
            table_name="security_listing_metrics",
            check_name="security_listing_metrics_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, security_id, as_of_date,
                           count(*) FILTER (WHERE is_latest_revision) AS latest_rows
                    FROM security_listing_metrics
                    GROUP BY 1, 2, 3
                    HAVING count(*) FILTER (WHERE is_latest_revision) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("security_listing_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="security_listing_metrics",
            table_name="security_listing_metrics",
            check_name="bad_security_listing_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_listing_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR symbol IS NULL OR symbol = ''
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR coalesce(round_lot_size, 0) < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("security_listing_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="security_listing_metrics",
            table_name="security_listing_metrics",
            check_name="security_listing_metric_flags_consistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM security_listing_metrics
                WHERE (is_listing_compliant AND financial_status_code IS DISTINCT FROM 'N')
                   OR (is_noncompliant AND (NOT has_financial_status OR financial_status_code = 'N'))
                   OR (NOT has_financial_status AND (is_deficient OR is_delinquent OR is_bankrupt OR is_noncompliant OR is_listing_compliant))
                   OR (has_financial_status AND financial_status_code IS NULL)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("security_listing_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="form144_intent",
            table_name="form144_intent",
            check_name="duplicate_form144_intent_accessions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT accession_number, count(*) AS row_count
                    FROM form144_intent
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_intent",),
        ),
        SqlQualityCheck(
            dataset_id="form144_intent",
            table_name="form144_intent",
            check_name="form144_intent_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source,
                        coalesce(seller_cik, seller_name_norm, ''),
                        coalesce(security_id, ''),
                        coalesce(approx_sale_date, notice_date, filing_date),
                        coalesce(security_title, ''),
                        count(*) FILTER (WHERE coalesce(is_latest, false)) AS latest_rows
                    FROM form144_intent
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) FILTER (WHERE coalesce(is_latest, false)) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_intent",),
        ),
        SqlQualityCheck(
            dataset_id="form144_intent",
            table_name="form144_intent",
            check_name="bad_form144_intent_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM form144_intent
                WHERE filing_id IS NULL OR filing_id = ''
                   OR accession_number IS NULL OR accession_number = ''
                   OR seller_name IS NULL OR seller_name = ''
                   OR seller_name_norm IS NULL OR seller_name_norm = ''
                   OR source IS NULL OR source = ''
                   OR source_url IS NULL OR source_url = ''
                   OR available_at IS NULL
                   OR coalesce(as_of_date, notice_date, filing_date, approx_sale_date) IS NULL
                   OR (security_id IS NULL AND issuer_cik IS NULL AND issuer_trading_symbol IS NULL)
                   OR coalesce(shares_proposed, 0) < 0
                   OR coalesce(aggregate_market_value, 0) < 0
                   OR coalesce(approx_price_per_share, 0) < 0
                   OR coalesce(restatement_seq, 0) < 0
                   OR (sale_window_end_date IS NOT NULL
                       AND approx_sale_date IS NOT NULL
                       AND sale_window_end_date < approx_sale_date)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_intent",),
        ),
        SqlQualityCheck(
            dataset_id="form144_to_form4_link",
            table_name="form144_to_form4_link",
            check_name="bad_form144_to_form4_links",
            sql="""
                SELECT count(*)::DOUBLE
                FROM form144_to_form4_link
                WHERE form144_filing_id IS NULL OR form144_filing_id = ''
                   OR insider_transaction_id IS NULL OR insider_transaction_id = ''
                   OR source IS NULL OR source = ''
                   OR match_confidence IS NULL
                   OR match_confidence < 0
                   OR match_confidence > 1
                   OR match_method IS NULL
                   OR match_method = ''
                   OR match_status NOT IN ('FULL', 'PARTIAL', 'EXCESS', 'UNKNOWN')
                   OR available_at IS NULL
                   OR as_of_date IS NULL
                   OR coalesce(days_between, 0) < 0
                   OR coalesce(shares_proposed, 0) < 0
                   OR coalesce(transaction_shares, 0) < 0
                   OR coalesce(execution_ratio, 0) < 0
                   OR coalesce(shares_matched, 0) < 0
                   OR coalesce(value_matched, 0) < 0
                   OR coalesce(share_match_ratio, 0) < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_to_form4_link",),
        ),
        SqlQualityCheck(
            dataset_id="form144_to_form4_link",
            table_name="form144_to_form4_link",
            check_name="orphan_form144_to_form4_links",
            sql="""
                SELECT count(*)::DOUBLE
                FROM form144_to_form4_link l
                LEFT JOIN form144_intent f ON f.filing_id = l.form144_filing_id
                LEFT JOIN insider_transaction t ON t.transaction_id = l.insider_transaction_id
                WHERE f.filing_id IS NULL
                   OR t.transaction_id IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("form144_to_form4_link", "form144_intent", "insider_transaction"),
        ),
        SqlQualityCheck(
            dataset_id="sec_blockholder_ownership",
            table_name="blockholder_filing",
            check_name="duplicate_blockholder_filings",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT accession_number, count(*) AS row_count
                    FROM blockholder_filing
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("blockholder_filing",),
        ),
        SqlQualityCheck(
            dataset_id="sec_blockholder_ownership",
            table_name="blockholder_filing",
            check_name="bad_blockholder_filing_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM blockholder_filing
                WHERE filing_id IS NULL
                   OR filing_id = ''
                   OR accession_number IS NULL
                   OR accession_number = ''
                   OR schedule_type NOT IN ('13D', '13D/A', '13G', '13G/A')
                   OR source_url IS NULL
                   OR source_url = ''
                   OR available_at IS NULL
            """,
            threshold=0.0,
            required_tables=("blockholder_filing",),
        ),
        SqlQualityCheck(
            dataset_id="sec_blockholder_ownership",
            table_name="blockholder_reporting_person",
            check_name="bad_blockholder_reporting_person_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM blockholder_reporting_person
                WHERE reporting_person_id IS NULL
                   OR reporting_person_id = ''
                   OR filing_id IS NULL
                   OR filing_id = ''
                   OR reporting_person_seq IS NULL
                   OR reporting_person_seq < 1
                   OR reporting_person_name IS NULL
                   OR reporting_person_name = ''
                   OR coalesce(percent_of_class, 0) < 0
                   OR coalesce(percent_of_class, 0) > 100
            """,
            threshold=0.0,
            required_tables=("blockholder_reporting_person",),
        ),
        SqlQualityCheck(
            dataset_id="sec_blockholder_ownership",
            table_name="blockholder_reporting_person",
            check_name="orphan_blockholder_reporting_persons",
            sql="""
                SELECT count(*)::DOUBLE
                FROM blockholder_reporting_person p
                LEFT JOIN blockholder_filing f ON f.filing_id = p.filing_id
                WHERE f.filing_id IS NULL
            """,
            threshold=0.0,
            required_tables=("blockholder_reporting_person", "blockholder_filing"),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_candidates",
            table_name="identifier_resolution_candidates",
            check_name="duplicate_identifier_resolution_candidates",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        source_dataset_id,
                        source_key_type,
                        source_key_value,
                        target_security_id,
                        coalesce(source_period, '') AS source_period,
                        match_method,
                        count(*) AS row_count
                    FROM identifier_resolution_candidates
                    GROUP BY 1, 2, 3, 4, 5, 6
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_candidates",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_candidates",
            table_name="identifier_resolution_candidates",
            check_name="bad_identifier_resolution_candidates",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_candidates
                WHERE source_dataset_id IS NULL
                   OR source_dataset_id = ''
                   OR source_key_type IS NULL
                   OR source_key_type = ''
                   OR source_key_value IS NULL
                   OR source_key_value = ''
                   OR target_security_id IS NULL
                   OR target_security_id = ''
                   OR confidence IS NULL
                   OR confidence < 0
                   OR confidence > 1
                   OR candidate_status NOT IN ('already_mapped', 'proposed', 'conflict')
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_candidates",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_candidates",
            table_name="identifier_resolution_candidates",
            check_name="orphan_identifier_resolution_targets",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_candidates c
                LEFT JOIN securities s
                  ON s.security_id = c.target_security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_candidates", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_candidates",
            table_name="identifier_resolution_candidates",
            check_name="missing_identifier_resolution_available_at",
            sql="SELECT count(*)::DOUBLE FROM identifier_resolution_candidates WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("identifier_resolution_candidates",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="identifier_resolution_decisions",
            check_name="duplicate_identifier_resolution_decisions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT candidate_id, decision_method, count(*) AS row_count
                    FROM identifier_resolution_decisions
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="identifier_resolution_decisions",
            check_name="bad_identifier_resolution_decisions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_decisions
                WHERE candidate_id IS NULL
                   OR candidate_id = ''
                   OR source_dataset_id IS NULL
                   OR source_dataset_id = ''
                   OR source_key_type IS NULL
                   OR source_key_type = ''
                   OR source_key_value IS NULL
                   OR source_key_value = ''
                   OR target_security_id IS NULL
                   OR target_security_id = ''
                   OR confidence IS NULL
                   OR confidence < 0
                   OR confidence > 1
                   OR candidate_status NOT IN ('already_mapped', 'proposed', 'conflict')
                   OR decision_status NOT IN ('accepted', 'rejected', 'needs_review')
                   OR decision_method IS NULL
                   OR decision_method = ''
                   OR decided_by IS NULL
                   OR decided_by = ''
                   OR decided_at IS NULL
                   OR effective_from IS NULL
                   OR as_of_date IS NULL
                   OR (decision_status = 'accepted' AND available_at IS NULL)
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions",),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="identifier_resolution_decisions",
            check_name="orphan_identifier_resolution_decision_candidates",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_decisions d
                LEFT JOIN identifier_resolution_candidates c
                  ON c.candidate_id = d.candidate_id
                WHERE c.candidate_id IS NULL
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions", "identifier_resolution_candidates"),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="identifier_resolution_decisions",
            check_name="orphan_identifier_resolution_decision_targets",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_decisions d
                LEFT JOIN securities s
                  ON s.security_id = d.target_security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="identifier_resolution_decisions",
            table_name="security_identifier_history",
            check_name="accepted_identifier_decisions_without_pit_identifier",
            sql="""
                SELECT count(*)::DOUBLE
                FROM identifier_resolution_decisions d
                LEFT JOIN security_identifier_history i
                  ON i.security_id = d.target_security_id
                 AND i.id_type = d.source_key_type
                 AND i.id_value = d.source_key_value
                 AND i.valid_from = d.effective_from
                 AND i.source = 'atx-impl identifier decision manager'
                WHERE d.decision_status = 'accepted'
                  AND d.source_key_type = 'CUSIP'
                  AND i.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("identifier_resolution_decisions", "security_identifier_history"),
        ),
        SqlQualityCheck(
            dataset_id="fred_macro",
            table_name="macro_observations",
            check_name="duplicate_macro_observations",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, series_id, observation_date, as_of_date, count(*) AS row_count
                    FROM macro_observations
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("macro_observations",),
        ),
        SqlQualityCheck(
            dataset_id="fred_macro",
            table_name="macro_series",
            check_name="macro_observations_without_series_metadata",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT source, series_id
                    FROM macro_observations
                ) o
                LEFT JOIN macro_series s
                  ON s.source = o.source
                 AND s.series_id = o.series_id
                WHERE s.series_id IS NULL
            """,
            threshold=0.0,
            required_tables=("macro_observations", "macro_series"),
        ),
        SqlQualityCheck(
            dataset_id="fred_macro",
            table_name="macro_observations",
            check_name="stale_daily_macro_observations",
            sql=f"""
                SELECT coalesce(max(date_diff('day', latest_date, current_date)), 0)::DOUBLE
                FROM (
                    SELECT series_id, max(observation_date) AS latest_date
                    FROM macro_observations
                    WHERE series_id IN ('DGS10', 'DGS2', 'VIXCLS')
                    GROUP BY series_id
                )
            """,
            threshold=float(daily_macro_stale_days),
            comparator="le",
            required_tables=("macro_observations",),
        ),
        SqlQualityCheck(
            dataset_id="fred_macro",
            table_name="macro_observations",
            check_name="stale_monthly_macro_observations",
            sql=f"""
                SELECT coalesce(max(date_diff('day', latest_date, current_date)), 0)::DOUBLE
                FROM (
                    SELECT series_id, max(observation_date) AS latest_date
                    FROM macro_observations
                    WHERE series_id IN ('FEDFUNDS', 'UNRATE', 'CPIAUCSL')
                    GROUP BY series_id
                )
            """,
            threshold=float(monthly_macro_stale_days),
            comparator="le",
            required_tables=("macro_observations",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="duplicate_feature_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT feature_set, feature_name, security_id, as_of_date, count(*) AS row_count
                    FROM feature_values
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_values",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="orphan_feature_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_values f
                LEFT JOIN securities s ON s.security_id = f.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_values", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="missing_feature_available_at",
            sql="SELECT count(*)::DOUBLE FROM feature_values WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("feature_values",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="missing_feature_input_hash",
            sql="SELECT count(*)::DOUBLE FROM feature_values WHERE input_hash IS NULL OR input_hash = ''",
            threshold=0.0,
            required_tables=("feature_values",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_definitions",
            check_name="duplicate_feature_definitions",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT feature_set, feature_name, count(*) AS row_count
                    FROM feature_definitions
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_definitions",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="feature_values_without_definition",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT feature_set, feature_name
                    FROM feature_values
                ) v
                LEFT JOIN feature_definitions d
                  ON d.feature_set = v.feature_set
                 AND d.feature_name = v.feature_name
                WHERE d.feature_name IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_values", "feature_definitions"),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_build_manifests",
            check_name="duplicate_feature_build_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT feature_set, run_id, count(*) AS row_count
                    FROM feature_build_manifests
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_build_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_build_manifests",
            check_name="bad_feature_build_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_build_manifests
                WHERE feature_set IS NULL
                   OR feature_set = ''
                   OR symbols_json IS NULL
                   OR feature_names_json IS NULL
                   OR input_tables_json IS NULL
                   OR input_row_count < 0
                   OR output_row_count < 0
                   OR feature_count <= 0
                   OR (input_min_as_of_date IS NOT NULL AND input_max_as_of_date < input_min_as_of_date)
                   OR (output_min_as_of_date IS NOT NULL AND output_max_as_of_date < output_min_as_of_date)
                   OR (min_available_at IS NOT NULL AND max_available_at < min_available_at)
            """,
            threshold=0.0,
            required_tables=("feature_build_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="equity_daily_features",
            table_name="feature_values",
            check_name="feature_values_without_build_manifest",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT DISTINCT feature_set, run_id
                    FROM feature_values
                    WHERE run_id IS NOT NULL
                      AND run_id <> ''
                ) v
                LEFT JOIN feature_build_manifests m
                  ON m.feature_set = v.feature_set
                 AND m.run_id = v.run_id
                WHERE m.manifest_id IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_values", "feature_build_manifests"),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_set_catalog",
            check_name="duplicate_feature_set_catalog_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT feature_set, count(*) AS row_count
                    FROM feature_set_catalog
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_set_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_set_catalog",
            check_name="bad_feature_set_catalog_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_set_catalog
                WHERE feature_set IS NULL
                   OR feature_set = ''
                   OR feature_family IS NULL
                   OR feature_family = ''
                   OR feature_count <= 0
                   OR dependency_count < 0
                   OR source_table_count < 0
                   OR derived_feature_dependency_count < 0
                   OR input_tables_json IS NULL
                   OR input_tables_json IN ('', '[]')
                   OR feature_names_json IS NULL
                   OR feature_names_json IN ('', '[]')
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("feature_set_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_dependency_edges",
            check_name="duplicate_feature_dependency_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT dependency_id, count(*) AS row_count
                    FROM feature_dependency_edges
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("feature_dependency_edges",),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_dependency_edges",
            check_name="bad_feature_dependency_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_dependency_edges
                WHERE dependency_id IS NULL
                   OR dependency_id = ''
                   OR feature_set IS NULL
                   OR feature_set = ''
                   OR feature_name IS NULL
                   OR feature_name = ''
                   OR dependency_type NOT IN ('source_table', 'derived_feature')
                   OR dependency_name IS NULL
                   OR dependency_name = ''
                   OR dependency_depth < 1
                   OR source IS NULL
                   OR source = ''
                   OR (dependency_type = 'source_table' AND dependency_feature_name IS NOT NULL)
                   OR (dependency_type = 'derived_feature' AND (
                       dependency_feature_set IS NULL
                       OR dependency_feature_name IS NULL
                   ))
            """,
            threshold=0.0,
            required_tables=("feature_dependency_edges",),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_dependency_edges",
            check_name="feature_definitions_without_dependency_edges",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_definitions d
                LEFT JOIN feature_dependency_edges e
                  ON e.feature_set = d.feature_set
                 AND e.feature_name = d.feature_name
                WHERE e.dependency_id IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_definitions", "feature_dependency_edges"),
        ),
        SqlQualityCheck(
            dataset_id="feature_lineage",
            table_name="feature_dependency_edges",
            check_name="derived_feature_edges_without_definition",
            sql="""
                SELECT count(*)::DOUBLE
                FROM feature_dependency_edges e
                LEFT JOIN feature_definitions d
                  ON d.feature_set = e.dependency_feature_set
                 AND d.feature_name = e.dependency_feature_name
                WHERE e.dependency_type = 'derived_feature'
                  AND d.feature_name IS NULL
            """,
            threshold=0.0,
            required_tables=("feature_dependency_edges", "feature_definitions"),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_expression_catalog",
            check_name="duplicate_alpha_expression_catalog",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT alpha_id, count(*) AS row_count
                    FROM alpha_expression_catalog
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("alpha_expression_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_expression_catalog",
            check_name="bad_alpha_expression_catalog_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_expression_catalog
                WHERE alpha_id IS NULL
                   OR alpha_id = ''
                   OR alpha_name IS NULL
                   OR alpha_name = ''
                   OR expression_sql IS NULL
                   OR expression_sql = ''
                   OR feature_set IS NULL
                   OR feature_set = ''
                   OR input_features_json IS NULL
                   OR input_features_json = ''
                   OR rebalance_frequency IS NULL
                   OR rebalance_frequency = ''
                   OR horizon_days < 1
                   OR direction NOT IN (-1, 1)
                   OR NOT is_point_in_time_safe
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("alpha_expression_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_signal_values",
            check_name="duplicate_alpha_signal_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT alpha_id, security_id, as_of_date, count(*) AS row_count
                    FROM alpha_signal_values
                    GROUP BY 1, 2, 3
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("alpha_signal_values",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_signal_values",
            check_name="bad_alpha_signal_value_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_signal_values
                WHERE alpha_signal_id IS NULL
                   OR alpha_signal_id = ''
                   OR alpha_id IS NULL
                   OR alpha_id = ''
                   OR security_id IS NULL
                   OR security_id = ''
                   OR as_of_date IS NULL
                   OR signal_value IS NULL
                   OR rank_value IS NULL
                   OR rank_value < 0
                   OR rank_value > 1
                   OR weight IS NULL
                   OR cross_section_count < 2
                   OR available_at IS NULL
                   OR input_hash IS NULL
                   OR input_hash = ''
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("alpha_signal_values",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_signal_values",
            check_name="alpha_signal_values_without_catalog",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_signal_values s
                LEFT JOIN alpha_expression_catalog c
                  ON c.alpha_id = s.alpha_id
                WHERE c.alpha_id IS NULL
            """,
            threshold=0.0,
            required_tables=("alpha_signal_values", "alpha_expression_catalog"),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_signal_values",
            check_name="alpha_signal_values_without_security",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_signal_values a
                LEFT JOIN securities s
                  ON s.security_id = a.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("alpha_signal_values", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_backtest_manifests",
            check_name="duplicate_alpha_backtest_manifests",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT backtest_id, count(*) AS row_count
                    FROM alpha_backtest_manifests
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("alpha_backtest_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_backtest_manifests",
            check_name="bad_alpha_backtest_manifest_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_backtest_manifests
                WHERE backtest_id IS NULL
                   OR backtest_id = ''
                   OR alpha_id IS NULL
                   OR alpha_id = ''
                   OR feature_set IS NULL
                   OR feature_set = ''
                   OR horizon_days < 1
                   OR signal_count < 1
                   OR security_count < 1
                   OR evaluation_days < 1
                   OR evaluated_signal_count < 1
                   OR (start_date IS NOT NULL AND end_date < start_date)
                   OR (min_available_at IS NOT NULL AND max_available_at < min_available_at)
                   OR hit_rate < 0
                   OR hit_rate > 1
                   OR params_json IS NULL
                   OR source IS NULL
                   OR source = ''
            """,
            threshold=0.0,
            required_tables=("alpha_backtest_manifests",),
        ),
        SqlQualityCheck(
            dataset_id="alpha_research",
            table_name="alpha_backtest_manifests",
            check_name="alpha_backtest_manifests_without_catalog",
            sql="""
                SELECT count(*)::DOUBLE
                FROM alpha_backtest_manifests b
                LEFT JOIN alpha_expression_catalog c
                  ON c.alpha_id = b.alpha_id
                WHERE c.alpha_id IS NULL
            """,
            threshold=0.0,
            required_tables=("alpha_backtest_manifests", "alpha_expression_catalog"),
        ),
        SqlQualityCheck(
            dataset_id="universe_memberships",
            table_name="universe_memberships",
            check_name="duplicate_universe_memberships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT universe_id, security_id, effective_date, as_of_date, count(*) AS row_count
                    FROM universe_memberships
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("universe_memberships",),
        ),
        SqlQualityCheck(
            dataset_id="universe_memberships",
            table_name="universe_memberships",
            check_name="orphan_universe_memberships",
            sql="""
                SELECT count(*)::DOUBLE
                FROM universe_memberships u
                LEFT JOIN securities s ON s.security_id = u.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("universe_memberships", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="universe_memberships",
            table_name="universe_memberships",
            check_name="missing_universe_available_at",
            sql="SELECT count(*)::DOUBLE FROM universe_memberships WHERE available_at IS NULL",
            threshold=0.0,
            required_tables=("universe_memberships",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_lake_exports",
            table_name="lake_export_files",
            check_name="duplicate_lake_export_files",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT export_run_id, object_name, count(*) AS row_count
                    FROM lake_export_files
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("lake_export_files",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_lake_exports",
            table_name="lake_export_runs",
            check_name="bad_lake_export_runs",
            sql="""
                SELECT count(*)::DOUBLE
                FROM lake_export_runs
                WHERE status NOT IN ('running', 'succeeded', 'failed')
                   OR object_count < 0
                   OR coalesce(total_rows, 0) < 0
                   OR coalesce(total_byte_count, 0) < 0
                   OR (status = 'succeeded' AND finished_at IS NULL)
                   OR (status = 'failed' AND coalesce(error_message, '') = '')
            """,
            threshold=0.0,
            required_tables=("lake_export_runs",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_lake_exports",
            table_name="lake_export_files",
            check_name="bad_lake_export_files",
            sql="""
                SELECT count(*)::DOUBLE
                FROM lake_export_files
                WHERE object_name IS NULL
                   OR object_name = ''
                   OR output_path IS NULL
                   OR output_path = ''
                   OR manifest_path IS NULL
                   OR manifest_path = ''
                   OR rows < 0
                   OR byte_count <= 0
                   OR sha256 IS NULL
                   OR length(sha256) <> 64
                   OR schema_sha256 IS NULL
                   OR length(schema_sha256) <> 64
            """,
            threshold=0.0,
            required_tables=("lake_export_files",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_lake_exports",
            table_name="lake_export_runs",
            check_name="incomplete_lake_export_runs",
            sql="""
                WITH file_totals AS (
                    SELECT
                        export_run_id,
                        count(*) AS file_count,
                        sum(rows) AS total_rows,
                        sum(byte_count) AS total_byte_count
                    FROM lake_export_files
                    GROUP BY 1
                )
                SELECT count(*)::DOUBLE
                FROM lake_export_runs r
                LEFT JOIN file_totals f ON f.export_run_id = r.export_run_id
                WHERE r.status = 'succeeded'
                  AND (
                      coalesce(f.file_count, 0) <> r.object_count
                      OR coalesce(f.total_rows, 0) <> coalesce(r.total_rows, 0)
                      OR coalesce(f.total_byte_count, 0) <> coalesce(r.total_byte_count, 0)
                  )
            """,
            threshold=0.0,
            required_tables=("lake_export_runs", "lake_export_files"),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="dataset_watermarks",
            check_name="bad_dataset_watermarks",
            sql="""
                SELECT count(*)::DOUBLE
                FROM dataset_watermarks
                WHERE dataset_id IS NULL
                   OR dataset_id = ''
                   OR watermark_name IS NULL
                   OR watermark_name = ''
                   OR watermark_value IS NULL
                   OR watermark_value = ''
            """,
            threshold=0.0,
            required_tables=("dataset_watermarks",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="dataset_watermarks",
            check_name="orphan_dataset_watermarks",
            sql="""
                SELECT count(*)::DOUBLE
                FROM dataset_watermarks w
                LEFT JOIN dataset_catalog d ON d.dataset_id = w.dataset_id
                WHERE d.dataset_id IS NULL
            """,
            threshold=0.0,
            required_tables=("dataset_watermarks", "dataset_catalog"),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="dataset_watermarks",
            check_name="missing_core_dataset_watermarks",
            sql="""
                WITH expected AS (
                    SELECT 'tbltickerhistory_daily' AS dataset_id, 'max_trade_date' AS watermark_name, count(*) AS row_count FROM tbltickerhistory_daily
                    UNION ALL
                    SELECT 'corporate_actions', 'max_ex_date', count(*) FROM corporate_actions
                    UNION ALL
                    SELECT 'corp_action_type_dim', 'max_updated_at', count(*) FROM corp_action_type_dim
                    UNION ALL
                    SELECT 'adjustment_factor_history', 'max_ex_date', count(*) FROM adjustment_factor_history
                    UNION ALL
                    SELECT 'adjustment_factor_history', 'max_available_at', count(*) FROM adjustment_factor_history
                    UNION ALL
                    SELECT 'daily_adjustment_factors', 'max_trade_date', count(*) FROM daily_adjustment_factors
                    UNION ALL
                    SELECT 'daily_adjustment_factors', 'max_as_of_date', count(*) FROM daily_adjustment_factors
                    UNION ALL
                    SELECT 'daily_adjustment_factors', 'max_available_at', count(*) FROM daily_adjustment_factors
                    UNION ALL
                    SELECT 'corporate_action_factor_reconciliation', 'max_ex_date', count(*) FROM corporate_action_factor_reconciliation
                    UNION ALL
                    SELECT 'corporate_action_factor_reconciliation', 'max_available_at', count(*) FROM corporate_action_factor_reconciliation
                    UNION ALL
                    SELECT 'shares_outstanding_history', 'max_effective_date', count(*) FROM shares_outstanding_history
                    UNION ALL
                    SELECT 'shares_outstanding_history', 'max_available_at', count(*) FROM shares_outstanding_history
                    UNION ALL
                    SELECT 'finra_short_interest', 'max_settlement_date', count(*) FROM finra_short_interest
                    UNION ALL
                    SELECT 'finra_short_interest_backfills', 'last_finished_at', count(*) FROM finra_short_interest_backfill_manifests
                    UNION ALL
                    SELECT 'finra_short_interest_features', 'max_as_of_date:finra_short_interest_v1', count(*) FROM feature_values WHERE feature_set = 'finra_short_interest_v1'
                    UNION ALL
                    SELECT 'sec_13f', 'source_period', count(*) FROM thirteenf_submissions
                    UNION ALL
                    SELECT 'sec_13f_ownership_features', 'max_report_period', count(*) FROM thirteenf_security_ownership
                    UNION ALL
                    SELECT 'sec_13f_ownership_features', 'max_available_at', count(*) FROM thirteenf_security_ownership
                    UNION ALL
                    SELECT 'sec_13f_ownership_features', 'max_as_of_date:sec_13f_ownership_v1', count(*) FROM feature_values WHERE feature_set = 'sec_13f_ownership_v1'
                    UNION ALL
                    SELECT 'sec_company_facts', 'max_available_at', count(*) FROM fundamental_points
                    UNION ALL
                    SELECT 'xbrl_concept_catalog', 'max_available_at', count(*) FROM xbrl_concept_catalog
                    UNION ALL
                    SELECT 'xbrl_concept_catalog', 'max_updated_at', count(*) FROM xbrl_concept_catalog
                    UNION ALL
                    SELECT 'xbrl_taxonomy', 'max_release_year', count(*) FROM xbrl_taxonomy_packages
                    UNION ALL
                    SELECT 'xbrl_taxonomy', 'max_source_loaded_at', count(*) FROM xbrl_taxonomy_relationships
                    UNION ALL
                    SELECT 'xbrl_dimensions', 'max_source_loaded_at', count(*) FROM xbrl_dimension_edges
                    UNION ALL
                    SELECT 'xbrl_fact_frames', 'max_available_at', count(*) FROM xbrl_fact_frames
                    UNION ALL
                    SELECT 'xbrl_fact_frames', 'max_updated_at', count(*) FROM xbrl_fact_frames
                    UNION ALL
                    SELECT 'xbrl_filing_contexts', 'max_acceptance_datetime', count(*) FROM xbrl_filing_contexts
                    UNION ALL
                    SELECT 'xbrl_filing_contexts', 'max_source_loaded_at', count(*) FROM xbrl_filing_contexts
                    UNION ALL
                    SELECT 'xbrl_filing_dimensions', 'max_acceptance_datetime', count(*) FROM xbrl_filing_dimensions
                    UNION ALL
                    SELECT 'xbrl_filing_facts', 'max_acceptance_datetime', count(*) FROM xbrl_filing_facts
                    UNION ALL
                    SELECT 'xbrl_filing_facts', 'max_source_loaded_at', count(*) FROM xbrl_filing_facts
                    UNION ALL
                    SELECT 'fundamental_fact_revisions', 'max_available_at', count(*) FROM fundamental_fact_revisions
                    UNION ALL
                    SELECT 'fundamental_fact_revisions', 'max_updated_at', count(*) FROM fundamental_fact_revisions
                    UNION ALL
                    SELECT 'fundamental_statement_map', 'max_updated_at', count(*) FROM fundamental_statement_map
                    UNION ALL
                    SELECT 'fundamental_statement_points', 'max_available_at', count(*) FROM fundamental_statement_points
                    UNION ALL
                    SELECT 'fundamental_statement_points', 'max_updated_at', count(*) FROM fundamental_statement_points
                    UNION ALL
                    SELECT 'fundamental_ttm_points', 'max_available_at', count(*) FROM fundamental_ttm_points
                    UNION ALL
                    SELECT 'fundamental_ttm_points', 'max_updated_at', count(*) FROM fundamental_ttm_points
                    UNION ALL
                    SELECT 'fundamental_periods', 'max_available_at', count(*) FROM fundamental_periods
                    UNION ALL
                    SELECT 'fundamental_periods', 'max_updated_at', count(*) FROM fundamental_periods
                    UNION ALL
                    SELECT 'sec_submissions', 'max_filing_date', count(*) FROM sec_submissions
                    UNION ALL
                    SELECT 'nasdaq_symbol_directory', 'max_as_of_date', count(*) FROM nasdaq_symbol_directory
                    UNION ALL
                    SELECT 'nasdaq_listing_events', 'max_effective_date', count(*) FROM nasdaq_listing_events
                    UNION ALL
                    SELECT 'nasdaq_listing_events', 'max_as_of_date', count(*) FROM nasdaq_listing_events
                    UNION ALL
                    SELECT 'nasdaq_listing_events', 'max_source_file_created_at', count(*) FROM nasdaq_listing_events
                    UNION ALL
                    SELECT 'listing_status_intervals', 'max_valid_from', count(*) FROM listing_status_intervals
                    UNION ALL
                    SELECT 'listing_status_intervals', 'max_available_at', count(*) FROM listing_status_intervals
                    UNION ALL
                    SELECT 'listing_status_intervals', 'max_last_evidence_as_of_date', count(*) FROM listing_status_intervals
                    UNION ALL
                    SELECT 'delist_code_dim', 'max_updated_at', count(*) FROM delist_code_dim
                    UNION ALL
                    SELECT 'delisting_events', 'max_delist_date', count(*) FROM delisting_events
                    UNION ALL
                    SELECT 'delisting_events', 'max_as_of_date', count(*) FROM delisting_events
                    UNION ALL
                    SELECT 'delisting_events', 'max_available_at', count(*) FROM delisting_events
                    UNION ALL
                    SELECT 'delisting_return_observations', 'max_delist_date', count(*) FROM delisting_return_observations
                    UNION ALL
                    SELECT 'delisting_return_observations', 'max_as_of_date', count(*) FROM delisting_return_observations
                    UNION ALL
                    SELECT 'delisting_return_observations', 'max_available_at', count(*) FROM delisting_return_observations
                    UNION ALL
                    SELECT 'fred_macro', 'max_observation_date', count(*) FROM macro_observations
                    UNION ALL
                    SELECT 'trading_calendar', 'max_trade_date', count(*) FROM trading_calendar
                    UNION ALL
                    SELECT 'universe_memberships', 'max_as_of_date', count(*) FROM universe_memberships
                    UNION ALL
                    SELECT 'equity_daily_features', 'max_as_of_date:equity_daily_v1', count(*) FROM feature_values WHERE feature_set = 'equity_daily_v1'
                    UNION ALL
                    SELECT 'sec_fundamental_features', 'max_as_of_date:sec_fundamentals_v1', count(*) FROM feature_values WHERE feature_set = 'sec_fundamentals_v1'
                    UNION ALL
                    SELECT 'feature_lineage', 'max_updated_at', count(*) FROM feature_dependency_edges
                    UNION ALL
                    SELECT 'alpha_research', 'max_as_of_date', count(*) FROM alpha_signal_values
                    UNION ALL
                    SELECT 'alpha_research', 'max_available_at', count(*) FROM alpha_signal_values
                    UNION ALL
                    SELECT 'alpha_research', 'max_backtest_end_date', count(*) FROM alpha_backtest_manifests
                    UNION ALL
                    SELECT 'warehouse_lake_exports', 'last_succeeded_export', count(*) FROM lake_export_runs WHERE status = 'succeeded'
                )
                SELECT count(*)::DOUBLE
                FROM expected e
                LEFT JOIN dataset_watermarks w
                  ON w.dataset_id = e.dataset_id
                 AND w.watermark_name = e.watermark_name
                WHERE e.row_count > 0
                  AND w.watermark_value IS NULL
            """,
            threshold=0.0,
            required_tables=(
                "dataset_watermarks",
                "tbltickerhistory_daily",
                "corporate_actions",
                "corp_action_type_dim",
                "adjustment_factor_history",
                "daily_adjustment_factors",
                "corporate_action_factor_reconciliation",
                "shares_outstanding_history",
                "finra_short_interest",
                "finra_short_interest_backfill_manifests",
                "thirteenf_submissions",
                "thirteenf_security_ownership",
                "fundamental_points",
                "xbrl_concept_catalog",
                "xbrl_taxonomy_packages",
                "xbrl_taxonomy_relationships",
                "xbrl_dimension_edges",
                "xbrl_fact_frames",
                "xbrl_filing_contexts",
                "xbrl_filing_dimensions",
                "xbrl_filing_facts",
                "fundamental_fact_revisions",
                "fundamental_statement_map",
                "fundamental_statement_points",
                "fundamental_ttm_points",
                "fundamental_periods",
                "sec_submissions",
                "nasdaq_symbol_directory",
                "nasdaq_listing_events",
                "listing_status_intervals",
                "delist_code_dim",
                "delisting_events",
                "delisting_return_observations",
                "macro_observations",
                "trading_calendar",
                "universe_memberships",
                "feature_values",
                "feature_dependency_edges",
                "alpha_signal_values",
                "alpha_backtest_manifests",
                "lake_export_runs",
            ),
        ),
        SqlQualityCheck(
            dataset_id="provider_parity_matrix",
            table_name="provider_parity_matrix",
            check_name="duplicate_provider_parity_domains",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT provider, provider_domain, count(*) AS row_count
                    FROM provider_parity_matrix
                    GROUP BY 1, 2
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("provider_parity_matrix",),
        ),
        SqlQualityCheck(
            dataset_id="provider_parity_matrix",
            table_name="provider_parity_matrix",
            check_name="bad_provider_parity_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM provider_parity_matrix
                WHERE provider IS NULL
                   OR provider = ''
                   OR provider_domain IS NULL
                   OR provider_domain = ''
                   OR warehouse_domain IS NULL
                   OR warehouse_domain = ''
                   OR institutional_grain IS NULL
                   OR institutional_grain = ''
                   OR institutional_keys_json IS NULL
                   OR institutional_keys_json IN ('', '[]')
                   OR pit_fields_json IS NULL
                   OR pit_fields_json IN ('', '[]')
                   OR factors_or_fields_json IS NULL
                   OR factors_or_fields_json IN ('', '[]')
                   OR open_substitute IS NULL
                   OR open_substitute = ''
                   OR warehouse_tables_json IS NULL
                   OR warehouse_tables_json IN ('', '[]')
                   OR source_urls_json IS NULL
                   OR source_urls_json IN ('', '[]')
                   OR parity_status NOT IN ('implemented', 'partial', 'planned', 'research_only')
            """,
            threshold=0.0,
            required_tables=("provider_parity_matrix",),
        ),
        SqlQualityCheck(
            dataset_id="provider_parity_matrix",
            table_name="provider_parity_matrix",
            check_name="provider_parity_rows_without_open_tables",
            sql="""
                SELECT count(*)::DOUBLE
                FROM provider_parity_matrix
                WHERE warehouse_tables_json NOT LIKE '%securities%'
                  AND warehouse_tables_json NOT LIKE '%equity_daily_bars%'
                  AND warehouse_tables_json NOT LIKE '%feature_values%'
                  AND warehouse_tables_json NOT LIKE '%fundamental_points%'
                  AND warehouse_tables_json NOT LIKE '%thirteenf_holdings%'
                  AND warehouse_tables_json NOT LIKE '%thirteenf_security_ownership%'
                  AND warehouse_tables_json NOT LIKE '%finra_short_interest%'
                  AND warehouse_tables_json NOT LIKE '%macro_observations%'
                  AND warehouse_tables_json NOT LIKE '%nasdaq_symbol_directory%'
                  AND warehouse_tables_json NOT LIKE '%nasdaq_listing_events%'
                  AND warehouse_tables_json NOT LIKE '%listing_status_intervals%'
                  AND warehouse_tables_json NOT LIKE '%dataset_catalog%'
            """,
            threshold=0.0,
            required_tables=("provider_parity_matrix",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_jobs",
            table_name="etl_job_definitions",
            check_name="bad_etl_job_retry_policy",
            sql="""
                SELECT count(*)::DOUBLE
                FROM etl_job_definitions
                WHERE max_retries < 0
                   OR retry_delay_seconds < 0
            """,
            threshold=0.0,
            required_tables=("etl_job_definitions",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_jobs",
            table_name="etl_job_runs",
            check_name="bad_etl_job_run_retry_metadata",
            sql="""
                SELECT count(*)::DOUBLE
                FROM etl_job_runs
                WHERE attempt_count < 0
                   OR max_retries < 0
                   OR retry_delay_seconds < 0
                   OR attempt_count > max_retries + 1
            """,
            threshold=0.0,
            required_tables=("etl_job_runs",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="field_catalog",
            check_name="missing_field_catalog_entries",
            sql="""
                SELECT count(*)::DOUBLE
                FROM duckdb_columns() c
                WHERE c.schema_name = 'main'
                  AND coalesce(c.internal, false) = false
                  AND c.table_name NOT LIKE 'duckdb_%'
                  AND c.table_name NOT LIKE 'sqlite_%'
                  AND c.table_name NOT LIKE 'pragma_%'
                  AND NOT EXISTS (
                      SELECT 1
                      FROM field_catalog f
                      WHERE f.table_name = c.table_name
                        AND f.field_name = c.column_name
                  )
            """,
            threshold=0.0,
            required_tables=("field_catalog",),
        ),
        SqlQualityCheck(
            dataset_id="warehouse_catalog",
            table_name="table_catalog",
            check_name="missing_table_catalog_entries",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT table_name AS object_name
                    FROM duckdb_tables()
                    WHERE schema_name = 'main'
                      AND coalesce(internal, false) = false
                    UNION
                    SELECT view_name AS object_name
                    FROM duckdb_views()
                    WHERE schema_name = 'main'
                      AND coalesce(internal, false) = false
                      AND view_name NOT LIKE 'duckdb_%'
                      AND view_name NOT LIKE 'sqlite_%'
                      AND view_name NOT LIKE 'pragma_%'
                ) o
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM table_catalog t
                    WHERE t.table_name = o.object_name
                )
            """,
            threshold=0.0,
            required_tables=("table_catalog",),
        ),
        # ── S1: reference classifications ────────────────────────────────────
        SqlQualityCheck(
            dataset_id="entity_classification",
            table_name="entity_classification",
            check_name="orphan_entity_classification_security_ids",
            sql="""
                SELECT count(*)::DOUBLE
                FROM entity_classification ec
                LEFT JOIN securities s ON s.security_id = ec.security_id
                WHERE s.security_id IS NULL
            """,
            threshold=0.0,
            required_tables=("entity_classification", "securities"),
        ),
        SqlQualityCheck(
            dataset_id="entity_classification",
            table_name="entity_classification",
            check_name="entity_classification_invalid_node_references",
            sql="""
                SELECT count(*)::DOUBLE
                FROM entity_classification ec
                LEFT JOIN taxonomy_node tn
                  ON tn.node_id = ec.node_id
                 AND tn.node_code = ec.node_code
                WHERE tn.node_id IS NULL
            """,
            threshold=0.0,
            required_tables=("entity_classification", "taxonomy_node"),
        ),
        SqlQualityCheck(
            dataset_id="entity_classification",
            table_name="entity_classification",
            check_name="entity_classification_multiple_open_intervals",
            # At most ONE open (valid_to IS NULL) interval per
            # (security_id, taxonomy_id) — covers primary SIC AND derived
            # FF12/NAICS rows, so cross-boundary reclassification that fails to
            # close a stale derived interval is caught here, not just for SIC.
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, taxonomy_id, count(*) AS open_count
                    FROM entity_classification
                    WHERE valid_to IS NULL
                    GROUP BY security_id, taxonomy_id
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            required_tables=("entity_classification",),
        ),
        # ── S2: estimates ─────────────────────────────────────────────────────
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_null_value",
            sql="SELECT count(*)::DOUBLE FROM est_actual WHERE value IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_invalid_fiscal_period",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_actual
                WHERE fiscal_period NOT IN ('Q1','Q2','Q3','Q4','FY')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_actual WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_eps_missing_basis",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_actual
                WHERE measure_code LIKE 'EPS%'
                  AND (basis IS NULL OR basis = '')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="est_actual",
            table_name="est_actual",
            check_name="est_actual_duplicate_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, measure_code, fiscal_year, fiscal_period,
                           accession_number, count(*) AS row_count
                    FROM est_actual
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_actual",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_guidance",
            table_name="est_guidance",
            check_name="est_guidance_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_guidance WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_guidance",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_guidance",
            table_name="est_guidance",
            check_name="est_guidance_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_guidance_id, count(*) AS row_count
                    FROM est_guidance
                    WHERE est_guidance_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_guidance",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_guidance",
            table_name="est_guidance",
            check_name="est_guidance_invalid_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_guidance
                WHERE period_end IS NULL
                   OR security_id IS NULL
                   OR measure_code IS NULL
                   OR (low IS NULL AND high IS NULL AND mid IS NULL)
                   OR (low IS NOT NULL AND high IS NOT NULL AND high < low)
                   OR (
                       extraction_confidence IS NOT NULL
                       AND (extraction_confidence < 0 OR extraction_confidence > 1)
                   )
                   OR (
                       guidance_type IS NOT NULL
                       AND guidance_type NOT IN ('POINT', 'RANGE', 'OPEN_LOW', 'OPEN_HIGH', 'QUAL')
                   )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_guidance",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_consensus WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_consensus_id, count(*) AS row_count
                    FROM est_consensus
                    WHERE est_consensus_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_invalid_stat_range",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_consensus
                WHERE (high IS NOT NULL AND low IS NOT NULL AND high < low)
                   OR (mean IS NOT NULL AND high IS NOT NULL AND mean > high)
                   OR (mean IS NOT NULL AND low IS NOT NULL AND mean < low)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_negative_counts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_consensus
                WHERE coalesce(num_estimates, 0) < 0
                   OR coalesce(num_up, 0) < 0
                   OR coalesce(num_down, 0) < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_consensus",
            table_name="est_consensus",
            check_name="est_consensus_invalid_stale_window",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_consensus
                WHERE stale_after_date IS NOT NULL
                  AND consensus_date IS NOT NULL
                  AND stale_after_date < consensus_date
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_consensus",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_recommendation WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_recommendation_id, count(*) AS row_count
                    FROM est_recommendation
                    WHERE est_recommendation_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_invalid_code",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation
                WHERE recommendation_code IS NOT NULL
                  AND recommendation_code NOT BETWEEN 1 AND 5
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_invalid_price_target",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation
                WHERE (price_target IS NOT NULL AND price_target <= 0)
                   OR (target_horizon_months IS NOT NULL AND target_horizon_months <= 0)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation",
            table_name="est_recommendation",
            check_name="est_recommendation_invalid_active_window",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation
                WHERE rating_date IS NOT NULL
                  AND (
                        (revision_date IS NOT NULL AND revision_date < rating_date)
                     OR (stop_date IS NOT NULL AND stop_date < rating_date)
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_recommendation_summary WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_recommendation_summary_id, count(*) AS row_count
                    FROM est_recommendation_summary
                    WHERE est_recommendation_summary_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_invalid_rating_mean",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation_summary
                WHERE (mean_recommendation IS NOT NULL AND (mean_recommendation < 1 OR mean_recommendation > 5))
                   OR (median_recommendation IS NOT NULL AND (median_recommendation < 1 OR median_recommendation > 5))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_negative_counts",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation_summary
                WHERE coalesce(strong_buy_count, 0) < 0
                   OR coalesce(buy_count, 0) < 0
                   OR coalesce(hold_count, 0) < 0
                   OR coalesce(underperform_count, 0) < 0
                   OR coalesce(sell_count, 0) < 0
                   OR coalesce(buy_equivalent_count, 0) < 0
                   OR coalesce(sell_equivalent_count, 0) < 0
                   OR coalesce(total_recommendations, 0) < 0
                   OR coalesce(price_target_count, 0) < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_bad_total_count",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation_summary
                WHERE total_recommendations IS NOT NULL
                  AND (
                    coalesce(strong_buy_count, 0)
                    + coalesce(buy_count, 0)
                    + coalesce(hold_count, 0)
                    + coalesce(underperform_count, 0)
                    + coalesce(sell_count, 0)
                  ) > total_recommendations
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_recommendation_summary",
            table_name="est_recommendation_summary",
            check_name="est_recommendation_summary_invalid_price_target_stats",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_recommendation_summary
                WHERE coalesce(mean_price_target, 1) <= 0
                   OR coalesce(median_price_target, 1) <= 0
                   OR coalesce(high_price_target, 1) <= 0
                   OR coalesce(low_price_target, 1) <= 0
                   OR (high_price_target IS NOT NULL AND low_price_target IS NOT NULL AND high_price_target < low_price_target)
                   OR (mean_price_target IS NOT NULL AND high_price_target IS NOT NULL AND mean_price_target > high_price_target)
                   OR (mean_price_target IS NOT NULL AND low_price_target IS NOT NULL AND mean_price_target < low_price_target)
                   OR (target_horizon_months IS NOT NULL AND target_horizon_months <= 0)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_recommendation_summary",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_security_link WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_security_link_id, count(*) AS row_count
                    FROM est_security_link
                    WHERE est_security_link_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_invalid_status_confidence",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_security_link
                WHERE link_status NOT IN ('accepted', 'conflict')
                   OR confidence < 0
                   OR confidence > 1
                   OR valid_from IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_conflicting_accepted_targets",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT provider, vendor_security_id_type, vendor_security_id
                    FROM est_security_link
                    WHERE link_status = 'accepted'
                    GROUP BY 1, 2, 3
                    HAVING count(DISTINCT target_security_id) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_security_link",
            table_name="est_security_link",
            check_name="est_security_link_orphan_target_security",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_security_link l
                LEFT JOIN securities s ON s.security_id = l.target_security_id
                WHERE l.link_status = 'accepted'
                  AND s.security_id IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_security_link", "securities"),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_detail",
            table_name="est_detail",
            check_name="est_detail_missing_available_at",
            sql="SELECT count(*)::DOUBLE FROM est_detail WHERE available_at IS NULL",
            threshold=0.0,
            comparator="eq",
            required_tables=("est_detail",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_detail",
            table_name="est_detail",
            check_name="est_detail_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_detail_id, count(*) AS row_count
                    FROM est_detail
                    WHERE est_detail_id IS NOT NULL
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_detail",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_detail",
            table_name="est_detail",
            check_name="est_detail_invalid_revision_window",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_detail
                WHERE announce_date IS NOT NULL
                  AND revision_date IS NOT NULL
                  AND revision_date < announce_date
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_detail",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_detail",
            table_name="est_detail",
            check_name="est_detail_invalid_stop_window",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_detail
                WHERE announce_date IS NOT NULL
                  AND stop_date IS NOT NULL
                  AND stop_date < announce_date
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_detail",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_period_dim",
            table_name="est_period_dim",
            check_name="est_period_dim_duplicate_id",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT est_period_id, count(*) AS row_count
                    FROM est_period_dim
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_period_dim",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_surprise",
            table_name="est_surprise",
            check_name="est_surprise_nonfinite_sue",
            # sue must be NULL (insufficient history) or a finite number
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_surprise
                WHERE sue IS NOT NULL
                  AND NOT isfinite(sue)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_surprise",),
            warn_if_missing=True,
        ),
        SqlQualityCheck(
            dataset_id="est_surprise",
            table_name="est_surprise",
            check_name="est_surprise_basis_mismatch_pct_null",
            sql="""
                SELECT count(*)::DOUBLE
                FROM est_surprise
                WHERE coalesce(basis_mismatch, false)
                  AND surprise_pct IS NOT NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("est_surprise",),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="press_release_facts",
            table_name="press_release_facts",
            check_name="bad_press_release_fact_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM press_release_facts
                WHERE security_id IS NULL
                   OR measure_code IS NULL
                   OR fiscal_period NOT IN ('Q1','Q2','Q3','Q4','FY')
                   OR period_end IS NULL
                   OR value IS NULL
                   OR available_at IS NULL
                   OR basis IS NULL
                   OR basis = ''
                   OR extraction_confidence IS NULL
                   OR extraction_confidence < 0
                   OR extraction_confidence > 1
                   OR source_item NOT LIKE '%2.02%'
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("press_release_facts",),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="press_release_facts",
            table_name="press_release_facts",
            check_name="press_release_no_lookahead",
            sql="""
                SELECT count(*)::DOUBLE
                FROM press_release_facts
                WHERE release_date IS NOT NULL
                  AND CAST(available_at AS DATE) < release_date
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("press_release_facts",),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="press_release_reconciliation",
            table_name="press_release_reconciliation",
            check_name="press_release_preliminary_vintage_retained",
            sql="""
                SELECT count(*)::DOUBLE
                FROM press_release_reconciliation r
                LEFT JOIN press_release_facts pr
                  ON pr.press_release_fact_id = r.press_release_fact_id
                WHERE pr.press_release_fact_id IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("press_release_reconciliation", "press_release_facts"),
            warn_if_missing=True,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="fundamental_statement_map_concept_coverage",
            sql="""
                SELECT count(DISTINCT item_id)::DOUBLE
                FROM fundamental_statement_map
                WHERE industry_template = 'ALL'
                  AND (
                      item_id BETWEEN 1001 AND 1043
                   OR item_id BETWEEN 1101 AND 1119
                   OR item_id BETWEEN 1201 AND 1223
                   OR item_id BETWEEN 1301 AND 1325
                   OR item_id BETWEEN 1401 AND 1427
                  )
            """,
            threshold=137.0,
            comparator="ge",
            required_tables=("fundamental_statement_map",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_statement_map",
            table_name="fundamental_statement_map",
            check_name="fundamental_statement_map_industry_overlay_coverage",
            sql="""
                SELECT count(DISTINCT item_id)::DOUBLE
                FROM fundamental_statement_map
                WHERE (
                       (industry_template = 'BK' AND item_id BETWEEN 1501 AND 1515)
                    OR (industry_template = 'IS' AND item_id BETWEEN 1601 AND 1610)
                    OR (industry_template = 'RT' AND item_id BETWEEN 1701 AND 1712)
                    OR (industry_template = 'UT' AND item_id BETWEEN 1801 AND 1805)
                    OR (industry_template = 'BD' AND item_id BETWEEN 1901 AND 1905)
                )
            """,
            threshold=47.0,
            comparator="eq",
            required_tables=("fundamental_statement_map",),
        ),
        SqlQualityCheck(
            dataset_id="industry_template",
            table_name="entity_industry_template",
            check_name="industry_template_exactly_one_route",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT
                        s.security_id,
                        count(e.route_id) AS route_count,
                        count(DISTINCT e.industry_template) AS template_count
                    FROM securities s
                    LEFT JOIN entity_industry_template e
                      ON e.security_id = s.security_id
                     AND e.is_latest_revision
                     AND e.valid_from <= current_date
                     AND coalesce(e.valid_to, DATE '9999-12-31') > current_date
                    GROUP BY s.security_id
                    HAVING route_count <> 1
                        OR template_count <> 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("securities", "entity_industry_template"),
            detail_sql="""
                SELECT
                    s.security_id,
                    count(e.route_id) AS route_count,
                    count(DISTINCT e.industry_template) AS template_count,
                    CAST(to_json(list(e.industry_template ORDER BY e.industry_template) FILTER (
                        WHERE e.industry_template IS NOT NULL
                    )) AS VARCHAR) AS templates_json
                FROM securities s
                LEFT JOIN entity_industry_template e
                  ON e.security_id = s.security_id
                 AND e.is_latest_revision
                 AND e.valid_from <= current_date
                 AND coalesce(e.valid_to, DATE '9999-12-31') > current_date
                GROUP BY s.security_id
                HAVING route_count <> 1
                    OR template_count <> 1
                ORDER BY s.security_id
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="industry_template",
            table_name="industry_template_coverage",
            check_name="industry_template_required_item_coverage",
            sql="""
                SELECT coalesce(sum(missing_item_count), 0)::DOUBLE
                FROM industry_template_coverage
                WHERE is_latest_revision
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("industry_template_coverage",),
            detail_sql="""
                SELECT
                    industry_template,
                    routed_entity_count,
                    required_item_count,
                    present_item_count,
                    not_available_item_count,
                    missing_item_count,
                    missing_item_ids_json
                FROM industry_template_coverage
                WHERE is_latest_revision
                  AND missing_item_count <> 0
                ORDER BY industry_template
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="filer_13f_cik_alias",
            table_name="filer_13f_cik_alias",
            check_name="filer_alias_overlapping_authoritative_windows",
            sql="""
                WITH auth AS (
                    SELECT alias_id, alias_cik, valid_from,
                           coalesce(valid_to, DATE '9999-12-31') AS valid_to
                    FROM filer_13f_cik_alias
                    WHERE alias_type IN ('SELF', 'SUBADVISOR', 'MA_CONTINUITY', 'MANUAL')
                )
                SELECT count(*)::DOUBLE
                FROM auth a
                JOIN auth b
                  ON a.alias_cik = b.alias_cik
                 AND a.alias_id <> b.alias_id
                 AND a.valid_from < b.valid_to
                 AND b.valid_from < a.valid_to
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("filer_13f_cik_alias",),
        ),
        SqlQualityCheck(
            dataset_id="filer_13f_cik_alias",
            table_name="filer_13f_cik_alias",
            check_name="filer_alias_confidence_out_of_range",
            sql="""
                SELECT count(*)::DOUBLE
                FROM filer_13f_cik_alias
                WHERE confidence < 0.0 OR confidence > 1.0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("filer_13f_cik_alias",),
        ),
        SqlQualityCheck(
            dataset_id="filer_13f_cik_alias",
            table_name="filer_13f_cik_alias",
            check_name="filer_alias_candidate_primary_has_self",
            sql="""
                SELECT count(*)::DOUBLE
                FROM filer_13f_cik_alias c
                WHERE c.alias_type = 'NAME_MATCH_CANDIDATE'
                  AND NOT EXISTS (
                      SELECT 1 FROM filer_13f_cik_alias s
                      WHERE s.alias_type = 'SELF' AND s.alias_cik = c.primary_cik
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("filer_13f_cik_alias",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_volume",
            table_name="offexchange_volume",
            check_name="offexchange_volume_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT symbol, mpid, venue_class, period_type, summary_start_date,
                           count(*) FILTER (WHERE is_latest) AS latest_rows
                    FROM offexchange_volume
                    GROUP BY symbol, mpid, venue_class, period_type, summary_start_date
                    HAVING count(*) FILTER (WHERE is_latest) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_volume",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_volume",
            table_name="offexchange_volume",
            check_name="offexchange_volume_bad_venue_class",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_volume
                WHERE venue_class NOT IN ('ATS', 'non_ATS')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_volume",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_security_period",
            table_name="offexchange_security_period",
            check_name="offexchange_security_period_pct_out_of_range",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_security_period
                WHERE ats_share_pct IS NOT NULL
                  AND (ats_share_pct < 0.0 OR ats_share_pct > 100.0)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_security_period",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_security_period",
            table_name="offexchange_security_period",
            check_name="offexchange_security_period_total_inconsistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_security_period
                WHERE abs(coalesce(total_share_quantity, 0)
                          - (coalesce(ats_share_quantity, 0) + coalesce(non_ats_share_quantity, 0))) > 1.0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_security_period",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_volume",
            table_name="finra_short_volume",
            check_name="finra_short_volume_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, symbol, trade_date, market_code,
                           count(*) FILTER (WHERE is_latest) AS latest_rows
                    FROM finra_short_volume
                    GROUP BY 1, 2, 3, 4
                    HAVING count(*) FILTER (WHERE is_latest) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("finra_short_volume",),
        ),
        SqlQualityCheck(
            dataset_id="finra_short_volume",
            table_name="finra_short_volume",
            check_name="finra_short_volume_bad_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM finra_short_volume
                WHERE symbol IS NULL
                   OR symbol = ''
                   OR trade_date IS NULL
                   OR market_code IS NULL
                   OR market_code = ''
                   OR available_at IS NULL
                   OR coalesce(short_volume, -1) < 0
                   OR coalesce(short_exempt_volume, -1) < 0
                   OR coalesce(total_volume, -1) < 0
                   OR short_volume > total_volume
                   OR short_exempt_volume > total_volume
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("finra_short_volume",),
        ),
        SqlQualityCheck(
            dataset_id="short_volume_metrics",
            table_name="short_volume_metrics",
            check_name="short_volume_metrics_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, symbol, trade_date,
                           count(*) FILTER (WHERE is_latest_revision) AS latest_rows
                    FROM short_volume_metrics
                    GROUP BY 1, 2, 3
                    HAVING count(*) FILTER (WHERE is_latest_revision) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_volume_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_volume_metrics",
            table_name="short_volume_metrics",
            check_name="short_volume_metrics_bad_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_volume_metrics
                WHERE symbol IS NULL
                   OR symbol = ''
                   OR trade_date IS NULL
                   OR available_at IS NULL
                   OR coalesce(short_volume, -1) < 0
                   OR coalesce(short_exempt_volume, -1) < 0
                   OR coalesce(total_volume, -1) < 0
                   OR short_volume > total_volume
                   OR short_exempt_volume > total_volume
                   OR short_volume_ratio < 0 OR short_volume_ratio > 1
                   OR short_exempt_ratio < 0 OR short_exempt_ratio > 1
                   OR short_volume_ratio_percentile < 0 OR short_volume_ratio_percentile > 1
                   OR short_exempt_ratio_percentile < 0 OR short_exempt_ratio_percentile > 1
                   OR dominant_market_share_pct < 0 OR dominant_market_share_pct > 100
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_volume_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_volume_metrics",
            table_name="short_volume_metrics",
            check_name="short_volume_high_flow_flag_consistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_volume_metrics
                WHERE is_high_short_flow
                  AND (
                      coalesce(short_volume_ratio_percentile, 0) < 0.90
                      OR coalesce(total_volume, 0) <= 0
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_volume_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_quality_report",
            table_name="offexchange_quality_report",
            check_name="offexchange_quality_report_multiple_latest_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, surface, input_source, period_type, period_start_date,
                           count(*) FILTER (WHERE is_latest_revision) AS latest_rows
                    FROM offexchange_quality_report
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) FILTER (WHERE is_latest_revision) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_quality_report",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_quality_report",
            table_name="offexchange_quality_report",
            check_name="offexchange_quality_report_bad_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_quality_report
                WHERE source IS NULL OR source = ''
                   OR surface NOT IN ('offexchange_volume', 'finra_short_volume')
                   OR period_type IS NULL OR period_type = ''
                   OR period_start_date IS NULL
                   OR available_at IS NULL
                   OR as_of_date IS NULL
                   OR period_end_date < period_start_date
                   OR coalesce(row_count, -1) < 0
                   OR coalesce(security_count, -1) < 0
                   OR coalesce(venue_or_market_count, -1) < 0
                   OR coalesce(restated_key_count, -1) < 0
                   OR coalesce(multiple_latest_key_count, -1) < 0
                   OR coalesce(bad_row_count, -1) < 0
                   OR coalesce(missing_available_at_count, -1) < 0
                   OR coalesce(high_short_flow_count, 0) < 0
                   OR coalesce(high_short_flow_count, 0) > coalesce(security_count, 0)
                   OR (short_volume_ratio IS NOT NULL AND (short_volume_ratio < 0 OR short_volume_ratio > 1))
                   OR (ats_share_pct IS NOT NULL AND (ats_share_pct < 0 OR ats_share_pct > 100))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_quality_report",),
        ),
        SqlQualityCheck(
            dataset_id="offexchange_quality_report",
            table_name="offexchange_quality_report",
            check_name="offexchange_quality_report_volume_consistent",
            sql="""
                SELECT count(*)::DOUBLE
                FROM offexchange_quality_report
                WHERE (surface = 'offexchange_volume'
                       AND abs(coalesce(total_volume, 0)
                               - (coalesce(ats_volume, 0) + coalesce(non_ats_volume, 0))) > 1.0)
                   OR (surface = 'finra_short_volume'
                       AND (coalesce(short_volume, 0) > coalesce(total_volume, 0)
                            OR coalesce(short_exempt_volume, 0) > coalesce(total_volume, 0)))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("offexchange_quality_report",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ratios",
            table_name="fundamental_ratios",
            check_name="duplicate_fundamental_ratio_natural_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, security_id, ratio_code, basis, period_end, count(*) AS n
                    FROM fundamental_ratios
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ratios",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ratios",
            table_name="fundamental_ratios",
            check_name="bad_fundamental_ratio_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ratios
                WHERE ratio_id IS NULL OR ratio_id = ''
                   OR source IS NULL OR source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR ratio_code IS NULL OR ratio_code = ''
                   OR ratio_category NOT IN ('profitability', 'leverage', 'cash_flow', 'payout', 'per_share', 'efficiency', 'growth', 'liquidity', 'health')
                   OR ratio_kind NOT IN ('ratio', 'level', 'per_share', 'growth', 'difference', 'score')
                   OR unit NOT IN ('ratio', 'currency', 'currency_per_share', 'score')
                   OR period_end IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR value IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ratios",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ratios",
            table_name="fundamental_ratios",
            check_name="non_finite_fundamental_ratio_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ratios
                WHERE value IS NOT NULL AND (isnan(value) OR isinf(value))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ratios",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_xbrl_metric",
            table_name="fundamental_xbrl_metric",
            check_name="duplicate_fundamental_xbrl_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT metric_id, count(*) AS n
                    FROM fundamental_xbrl_metric
                    GROUP BY 1
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_xbrl_metric",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_xbrl_metric",
            table_name="fundamental_xbrl_metric",
            check_name="multiple_latest_fundamental_xbrl_metric_per_key",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT security_id, canonical_metric, period_type,
                           coalesce(CAST(period_start AS VARCHAR), ''), period_end, count(*) AS n
                    FROM fundamental_xbrl_metric
                    WHERE is_latest_revision
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_xbrl_metric",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_xbrl_metric",
            table_name="fundamental_xbrl_metric",
            check_name="bad_fundamental_xbrl_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_xbrl_metric
                WHERE metric_id IS NULL OR metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR canonical_metric IS NULL OR canonical_metric = ''
                   OR concept IS NULL OR concept = ''
                   OR period_type NOT IN ('instant', 'duration')
                   OR period_end IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_xbrl_metric",),
        ),
        SqlQualityCheck(
            dataset_id="fundamental_xbrl_metric",
            table_name="fundamental_xbrl_metric_ratio_universe_gap",
            check_name="xbrl_metric_covers_fundamental_ratio_universe",
            sql="""
                SELECT ratio_minus_xbrl_security_count::DOUBLE
                FROM fundamental_xbrl_metric_ratio_universe_gap
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_xbrl_metric_ratio_universe_gap",),
            detail_sql="""
                SELECT *
                FROM fundamental_xbrl_metric_ratio_universe_gap
                WHERE ratio_minus_xbrl_security_count > 0
            """,
        ),
        SqlQualityCheck(
            dataset_id="fundamental_ratios",
            table_name="fundamental_ratios",
            check_name="ratio_kind_division_consistency",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fundamental_ratios
                WHERE ratio_kind IN ('ratio', 'per_share')
                  AND denominator_value IS NOT NULL
                  AND numerator_value IS NOT NULL
                  AND denominator_value <> 0
                  AND abs(value - (numerator_value / denominator_value)) > 1e-6 * (1 + abs(value))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fundamental_ratios",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="duplicate_valuation_multiple_natural_keys",
            sql="""
                SELECT count(*)::DOUBLE
                FROM (
                    SELECT source, market_cap_source, security_id, trade_date, formula_code, count(*) AS n
                    FROM valuation_multiples
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="bad_valuation_multiple_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE valuation_multiple_id IS NULL OR valuation_multiple_id = ''
                   OR source IS NULL OR source = ''
                   OR market_cap_source IS NULL OR market_cap_source = ''
                   OR security_id IS NULL OR security_id = ''
                   OR trade_date IS NULL
                   OR formula_code IS NULL OR formula_code = ''
                   OR category <> 'valuation'
                   OR kind NOT IN ('ratio', 'difference')
                   OR unit NOT IN ('ratio', 'currency')
                   OR period_end IS NULL
                   OR period_end > trade_date
                   OR value IS NULL
                   OR numerator_value IS NULL
                   OR denominator_value IS NULL
                   OR price IS NULL
                   OR market_cap IS NULL
                   OR is_meaningful IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR market_cap_available_at IS NULL
                   OR price_available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="non_finite_valuation_multiple_values",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE (value IS NOT NULL AND (isnan(value) OR isinf(value)))
                   OR (numerator_value IS NOT NULL AND (isnan(numerator_value) OR isinf(numerator_value)))
                   OR (denominator_value IS NOT NULL AND (isnan(denominator_value) OR isinf(denominator_value)))
                   OR (price IS NOT NULL AND (isnan(price) OR isinf(price)))
                   OR (market_cap IS NOT NULL AND (isnan(market_cap) OR isinf(market_cap)))
                   OR (enterprise_value IS NOT NULL AND (isnan(enterprise_value) OR isinf(enterprise_value)))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="valuation_multiple_arithmetic_consistency",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE (
                        kind = 'ratio'
                    AND denominator_value IS NOT NULL
                    AND numerator_value IS NOT NULL
                    AND denominator_value <> 0
                    AND abs(value - (numerator_value / denominator_value)) > 1e-6 * (1 + abs(value))
                )
                   OR (
                        formula_code = 'enterprise_value'
                    AND abs(value - (numerator_value - denominator_value)) > 1e-6 * (1 + abs(value))
                )
                   OR (
                        formula_code = 'enterprise_value'
                    AND enterprise_value IS NOT NULL
                    AND abs(enterprise_value - value) > 1e-6 * (1 + abs(enterprise_value))
                )
                   OR (
                        formula_code IN ('ev_to_ebitda', 'ev_to_sales')
                    AND enterprise_value IS NOT NULL
                    AND numerator_value IS NOT NULL
                    AND abs(enterprise_value - numerator_value) > 1e-6 * (1 + abs(enterprise_value))
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="valuation_multiple_non_positive_denominator_meaningfulness",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE coalesce(is_meaningful, false)
                  AND (
                        (kind = 'ratio' AND denominator_value <= 0)
                     OR market_cap <= 0
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
        ),
        SqlQualityCheck(
            dataset_id="valuation_multiples",
            table_name="valuation_multiples",
            check_name="stale_price_fundamental_gap_days",
            sql=f"""
                SELECT count(*)::DOUBLE
                FROM valuation_multiples
                WHERE trade_date IS NOT NULL
                  AND period_end IS NOT NULL
                  AND date_diff('day', period_end, trade_date) > {valuation_stale_gap_days}
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_multiples",),
            failure_status="warning",
            detail_sql=f"""
                SELECT
                    security_id,
                    symbol,
                    trade_date,
                    period_end,
                    formula_code,
                    date_diff('day', period_end, trade_date) AS gap_days,
                    available_at
                FROM valuation_multiples
                WHERE trade_date IS NOT NULL
                  AND period_end IS NOT NULL
                  AND date_diff('day', period_end, trade_date) > {valuation_stale_gap_days}
                ORDER BY gap_days DESC, security_id, formula_code
                LIMIT 20
            """,
        ),
        SqlQualityCheck(
            dataset_id="valuation_overlap_slice",
            table_name="valuation_overlap_slice",
            check_name="bad_valuation_overlap_slice_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM valuation_overlap_slice
                WHERE overlap_slice_id IS NULL OR overlap_slice_id = ''
                   OR source IS NULL OR source = ''
                   OR market_cap_sources_json IS NULL
                   OR symbol_scope_json IS NULL
                   OR numerator_security_count < 0
                   OR denominator_security_count < 0
                   OR valuation_row_count < 0
                   OR (coverage_ratio IS NOT NULL AND (coverage_ratio < 0 OR coverage_ratio > 1))
                   OR stale_price_fundamental_gap_days < 0
                   OR stale_valuation_row_count < 0
                   OR denominator_definition IS NULL OR denominator_definition = ''
                   OR details_json IS NULL OR details_json = ''
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("valuation_overlap_slice",),
        ),
        SqlQualityCheck(
            dataset_id="fact_disagreement",
            table_name="fact_disagreement",
            check_name="fact_disagreement_agreement_ratio",
            sql="""
                SELECT coalesce(
                    count(*) FILTER (WHERE agreement_status = 'agrees')::DOUBLE
                    / nullif(count(*)::DOUBLE, 0.0),
                    1.0
                )
                FROM fact_disagreement
            """,
            threshold=0.99,
            comparator="ge",
            required_tables=("fact_disagreement",),
            detail_sql="""
                SELECT
                    vendor,
                    security_id,
                    canonical_code,
                    basis,
                    period_end,
                    warehouse_value,
                    vendor_value,
                    absolute_difference,
                    relative_difference,
                    agreement_status
                FROM fact_disagreement
                WHERE agreement_status <> 'agrees'
                ORDER BY agreement_status, vendor, security_id, canonical_code, period_end
                LIMIT 25
            """,
            severity="critical",
        ),
        SqlQualityCheck(
            dataset_id="fact_disagreement",
            table_name="fact_disagreement",
            check_name="bad_fact_disagreement_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM fact_disagreement
                WHERE disagreement_id IS NULL OR disagreement_id = ''
                   OR source IS NULL OR source = ''
                   OR baseline_source IS NULL OR baseline_source = ''
                   OR vendor IS NULL OR vendor = ''
                   OR security_id IS NULL OR security_id = ''
                   OR item_id IS NULL
                   OR canonical_code IS NULL OR canonical_code = ''
                   OR basis IS NULL OR basis = ''
                   OR period_end IS NULL
                   OR vendor_value IS NULL
                   OR agreement_status NOT IN ('agrees', 'disagrees', 'missing_warehouse')
                   OR vintage_status IS NULL OR vintage_status = ''
                   OR is_latest_revision IS NULL
                   OR tolerance_abs < 0
                   OR tolerance_rel < 0
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR input_lineage_json IS NULL OR input_lineage_json = ''
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("fact_disagreement",),
        ),
        SqlQualityCheck(
            dataset_id="short_interest_metrics",
            table_name="short_interest_metrics",
            check_name="duplicate_short_interest_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, security_id, settlement_date
                    FROM short_interest_metrics
                    GROUP BY 1, 2, 3 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_interest_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_interest_metrics",
            table_name="short_interest_metrics",
            check_name="bad_short_interest_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_interest_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR settlement_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR (days_to_cover_percentile IS NOT NULL
                       AND (days_to_cover_percentile < 0 OR days_to_cover_percentile > 1))
                   OR (short_interest_change_pct_percentile IS NOT NULL
                       AND (short_interest_change_pct_percentile < 0 OR short_interest_change_pct_percentile > 1))
                   OR (short_pressure_score IS NOT NULL
                       AND (short_pressure_score < 0 OR short_pressure_score > 100))
                   OR (average_daily_volume_percentile IS NOT NULL
                       AND (average_daily_volume_percentile < 0 OR average_daily_volume_percentile > 1))
                   OR (liquid_short_pressure_score IS NOT NULL
                       AND (liquid_short_pressure_score < 0 OR liquid_short_pressure_score > 100))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_interest_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_interest_metrics",
            table_name="short_interest_metrics",
            check_name="persistent_short_pressure_without_squeeze",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_interest_metrics
                WHERE coalesce(is_persistent_short_pressure, false)
                  AND NOT coalesce(is_squeeze_candidate, false)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_interest_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="short_interest_metrics",
            table_name="short_interest_metrics",
            check_name="liquid_short_pressure_without_tradeability",
            sql="""
                SELECT count(*)::DOUBLE
                FROM short_interest_metrics
                WHERE coalesce(is_liquid_short_pressure, false)
                  AND (
                      NOT coalesce(is_squeeze_candidate, false)
                      OR coalesce(average_daily_volume, 0) < 50000
                      OR coalesce(current_short_position, 0) < 100000
                      OR coalesce(short_pressure_score, 0) < 70
                      OR liquid_short_pressure_score IS NULL
                  )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("short_interest_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="macro_metrics",
            table_name="macro_metrics",
            check_name="duplicate_macro_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, series_id, observation_date
                    FROM macro_metrics
                    GROUP BY 1, 2, 3 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("macro_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="macro_metrics",
            table_name="macro_metrics",
            check_name="bad_macro_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM macro_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR series_id IS NULL OR series_id = ''
                   OR observation_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("macro_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="macro_metrics",
            table_name="macro_metrics",
            check_name="missing_real_fedfunds_when_inputs_available",
            sql="""
                SELECT count(*)::DOUBLE
                FROM macro_metrics fed
                JOIN macro_metrics cpi
                  ON cpi.source = fed.source
                 AND cpi.series_id = 'CPIAUCSL'
                 AND cpi.observation_date = fed.observation_date
                 AND cpi.yoy_growth IS NOT NULL
                LEFT JOIN macro_metrics real
                  ON real.source = fed.source
                 AND real.series_id = 'REAL_FEDFUNDS'
                 AND real.observation_date = fed.observation_date
                WHERE fed.series_id = 'FEDFUNDS'
                  AND real.metric_id IS NULL
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("macro_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="macro_metrics",
            table_name="macro_metrics",
            check_name="missing_sahm_rule_when_unrate_available",
            sql="""
                SELECT count(*)::DOUBLE
                FROM macro_metrics u
                LEFT JOIN macro_metrics sahm
                  ON sahm.source = u.source
                 AND sahm.series_id = 'SAHM_RULE'
                 AND sahm.observation_date = u.observation_date
                WHERE u.series_id = 'UNRATE'
                  AND sahm.metric_id IS NULL
                  AND (
                      SELECT count(*) FROM macro_metrics u2
                      WHERE u2.source = u.source
                        AND u2.series_id = 'UNRATE'
                        AND u2.observation_date <= u.observation_date
                  ) >= 14
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("macro_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="equity_price_metrics",
            table_name="equity_price_metrics",
            check_name="duplicate_equity_price_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, security_id, trade_date
                    FROM equity_price_metrics
                    GROUP BY 1, 2, 3 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("equity_price_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="equity_price_metrics",
            table_name="equity_price_metrics",
            check_name="bad_equity_price_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM equity_price_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR trade_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR (pct_from_high_252d IS NOT NULL AND pct_from_high_252d > 1e-9)
                   OR (realized_vol_20d IS NOT NULL AND realized_vol_20d < 0)
                   OR (avg_dollar_volume_21d IS NOT NULL AND avg_dollar_volume_21d < 0)
                   OR (amihud_illiquidity_21d IS NOT NULL AND amihud_illiquidity_21d < 0)
                   OR (max_drawdown_126d IS NOT NULL AND max_drawdown_126d > 1e-9)
                   OR (downside_deviation_60d IS NOT NULL AND downside_deviation_60d < 0)
                   OR (market_return_ew IS NOT NULL AND NOT isfinite(market_return_ew))
                   OR (beta_60d IS NOT NULL AND NOT isfinite(beta_60d))
                   OR (market_correlation_60d IS NOT NULL AND NOT isfinite(market_correlation_60d))
                   OR (market_correlation_60d IS NOT NULL AND (market_correlation_60d < -1.0000001 OR market_correlation_60d > 1.0000001))
                   OR (idiosyncratic_vol_60d IS NOT NULL AND NOT isfinite(idiosyncratic_vol_60d))
                   OR (idiosyncratic_vol_60d IS NOT NULL AND idiosyncratic_vol_60d < 0)
                   OR (daily_return_cs_pct_rank IS NOT NULL AND (NOT isfinite(daily_return_cs_pct_rank) OR daily_return_cs_pct_rank < 0 OR daily_return_cs_pct_rank > 1.0000001))
                   OR (momentum_21d_cs_pct_rank IS NOT NULL AND (NOT isfinite(momentum_21d_cs_pct_rank) OR momentum_21d_cs_pct_rank < 0 OR momentum_21d_cs_pct_rank > 1.0000001))
                   OR (realized_vol_20d_cs_pct_rank IS NOT NULL AND (NOT isfinite(realized_vol_20d_cs_pct_rank) OR realized_vol_20d_cs_pct_rank < 0 OR realized_vol_20d_cs_pct_rank > 1.0000001))
                   OR (dollar_volume_cs_pct_rank IS NOT NULL AND (NOT isfinite(dollar_volume_cs_pct_rank) OR dollar_volume_cs_pct_rank < 0 OR dollar_volume_cs_pct_rank > 1.0000001))
                   OR (amihud_illiquidity_21d_cs_pct_rank IS NOT NULL AND (NOT isfinite(amihud_illiquidity_21d_cs_pct_rank) OR amihud_illiquidity_21d_cs_pct_rank < 0 OR amihud_illiquidity_21d_cs_pct_rank > 1.0000001))
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("equity_price_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_position_metrics",
            table_name="thirteenf_position_metrics",
            check_name="duplicate_thirteenf_position_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, manager_id, security_id, report_period
                    FROM thirteenf_position_metrics
                    GROUP BY 1, 2, 3, 4 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_position_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_position_metrics",
            table_name="thirteenf_position_metrics",
            check_name="bad_thirteenf_position_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_position_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR manager_id IS NULL OR manager_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR report_period IS NULL
                   OR as_of_date IS NULL
                   OR position_action NOT IN ('NEW', 'ADDED', 'TRIMMED', 'UNCHANGED', 'EXITED')
                   OR (is_closed_position AND shares_held <> 0)
                   OR (is_new_position AND shares_held_prev IS NOT NULL)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_position_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_option_metrics",
            table_name="thirteenf_option_metrics",
            check_name="duplicate_thirteenf_option_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT
                        source,
                        coalesce(security_id, ''),
                        coalesce(cusip, ''),
                        report_period,
                        coalesce(source_period, '')
                    FROM thirteenf_option_metrics
                    GROUP BY 1, 2, 3, 4, 5 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_option_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_option_metrics",
            table_name="thirteenf_option_metrics",
            check_name="bad_thirteenf_option_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_option_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR source IS NULL OR source = ''
                   OR (coalesce(security_id, '') = '' AND coalesce(cusip, '') = '')
                   OR report_period IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR option_bias NOT IN ('CALL_HEAVY', 'PUT_HEAVY', 'BALANCED', 'NO_OPTIONS')
                   OR option_manager_count < 0
                   OR call_manager_count < 0
                   OR put_manager_count < 0
                   OR option_position_count < 1
                   OR call_position_count < 0
                   OR put_position_count < 0
                   OR option_position_count <> call_position_count + put_position_count
                   OR call_share_quantity < 0
                   OR put_share_quantity < 0
                   OR call_value_usd < 0
                   OR put_value_usd < 0
                   OR common_share_quantity < 0
                   OR common_value_usd < 0
                   OR put_call_share_ratio < 0
                   OR put_call_value_ratio < 0
                   OR call_to_common_share_pct < 0
                   OR put_to_common_share_pct < 0
                   OR option_to_common_value_pct < 0
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_option_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_option_metrics",
            table_name="thirteenf_option_metrics",
            check_name="mismatched_thirteenf_option_bias",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_option_metrics
                WHERE (
                        coalesce(call_share_quantity, 0) > coalesce(put_share_quantity, 0)
                        AND option_bias <> 'CALL_HEAVY'
                    )
                   OR (
                        coalesce(put_share_quantity, 0) > coalesce(call_share_quantity, 0)
                        AND option_bias <> 'PUT_HEAVY'
                    )
                   OR (
                        coalesce(call_share_quantity, 0) = coalesce(put_share_quantity, 0)
                        AND coalesce(call_share_quantity, 0) > 0
                        AND option_bias <> 'BALANCED'
                    )
                   OR (
                        coalesce(call_share_quantity, 0) = 0
                        AND coalesce(put_share_quantity, 0) = 0
                        AND option_bias <> 'NO_OPTIONS'
                    )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_option_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_concentration_metrics",
            table_name="thirteenf_concentration_metrics",
            check_name="duplicate_thirteenf_concentration_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT
                        source,
                        coalesce(security_id, ''),
                        coalesce(cusip, ''),
                        report_period,
                        coalesce(source_period, '')
                    FROM thirteenf_concentration_metrics
                    GROUP BY 1, 2, 3, 4, 5 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_concentration_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_concentration_metrics",
            table_name="thirteenf_concentration_metrics",
            check_name="bad_thirteenf_concentration_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_concentration_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR source IS NULL OR source = ''
                   OR (coalesce(security_id, '') = '' AND coalesce(cusip, '') = '')
                   OR report_period IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR filing_count < 1
                   OR holder_count < 1
                   OR common_value_usd < 0
                   OR common_share_quantity < 0
                   OR top_holder_value_usd < 0
                   OR top_holder_share_quantity < 0
                   OR top_holder_value_pct < 0 OR top_holder_value_pct > 1.0000001
                   OR top_holder_share_pct < 0 OR top_holder_share_pct > 1.0000001
                   OR top_3_holder_value_pct < 0 OR top_3_holder_value_pct > 1.0000001
                   OR top_5_holder_value_pct < 0 OR top_5_holder_value_pct > 1.0000001
                   OR top_10_holder_value_pct < 0 OR top_10_holder_value_pct > 1.0000001
                   OR value_hhi < 0 OR value_hhi > 1.0000001
                   OR share_hhi < 0 OR share_hhi > 1.0000001
                   OR effective_holder_count_value < 1
                   OR effective_holder_count_share < 1
                   OR concentration_bucket NOT IN ('DISPERSED', 'MODERATE', 'CONCENTRATED', 'HIGHLY_CONCENTRATED')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_concentration_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="thirteenf_concentration_metrics",
            table_name="thirteenf_concentration_metrics",
            check_name="mismatched_thirteenf_concentration_ordering",
            sql="""
                SELECT count(*)::DOUBLE
                FROM thirteenf_concentration_metrics
                WHERE top_holder_value_pct > top_3_holder_value_pct + 1e-9
                   OR top_3_holder_value_pct > top_5_holder_value_pct + 1e-9
                   OR top_5_holder_value_pct > top_10_holder_value_pct + 1e-9
                   OR top_10_holder_value_pct > 1.0000001
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("thirteenf_concentration_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_dividend_metrics",
            table_name="corporate_action_dividend_metrics",
            check_name="duplicate_corporate_action_dividend_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, security_id, ex_date
                    FROM corporate_action_dividend_metrics
                    GROUP BY 1, 2, 3 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_dividend_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_dividend_metrics",
            table_name="corporate_action_dividend_metrics",
            check_name="bad_corporate_action_dividend_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_dividend_metrics
                WHERE metric_id IS NULL OR metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR ex_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR (dividend_yield_spot IS NOT NULL AND dividend_yield_spot < 0)
                   OR (ttm_dividend_count IS NOT NULL AND ttm_dividend_count < 1)
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_dividend_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_split_metrics",
            table_name="corporate_action_split_metrics",
            check_name="duplicate_corporate_action_split_metric_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT source, factor_source, daily_adjustment_source, coalesce(bar_source, ''), event_ref_id
                    FROM corporate_action_split_metrics
                    GROUP BY 1, 2, 3, 4, 5 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_split_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_split_metrics",
            table_name="corporate_action_split_metrics",
            check_name="bad_corporate_action_split_metric_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_split_metrics
                WHERE split_metric_id IS NULL OR split_metric_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR event_ref_id IS NULL OR event_ref_id = ''
                   OR ex_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR factor_price <= 0
                   OR factor_shares <= 0
                   OR reconciliation_status NOT IN ('RECONCILED', 'MISMATCH', 'MISSING_DAILY_FACTOR')
                   OR (is_reconciled AND reconciliation_status <> 'RECONCILED')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_split_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_split_metrics",
            table_name="corporate_action_split_metrics",
            check_name="mismatched_corporate_action_split_factors",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_split_metrics
                WHERE reconciliation_status = 'MISMATCH'
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_split_metrics",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_factor_reconciliation",
            table_name="corporate_action_factor_reconciliation",
            check_name="duplicate_corporate_action_factor_reconciliation_keys",
            sql="""
                SELECT count(*)::DOUBLE FROM (
                    SELECT
                        source,
                        factor_source,
                        daily_adjustment_source,
                        coalesce(bar_source, ''),
                        event_ref_id,
                        event_type
                    FROM corporate_action_factor_reconciliation
                    GROUP BY 1, 2, 3, 4, 5, 6 HAVING count(*) > 1
                )
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_factor_reconciliation",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_factor_reconciliation",
            table_name="corporate_action_factor_reconciliation",
            check_name="bad_corporate_action_factor_reconciliation_rows",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_factor_reconciliation
                WHERE reconciliation_id IS NULL OR reconciliation_id = ''
                   OR security_id IS NULL OR security_id = ''
                   OR event_ref_id IS NULL OR event_ref_id = ''
                   OR event_type IS NULL OR event_type = ''
                   OR ex_date IS NULL
                   OR as_of_date IS NULL
                   OR available_at IS NULL
                   OR factor_price <= 0
                   OR factor_shares <= 0
                   OR same_day_event_count < 1
                   OR reconciliation_status NOT IN (
                       'RECONCILED',
                       'MISMATCH',
                       'MISSING_DAILY_FACTOR',
                       'COMPOUND_EVENT',
                       'UNSUPPORTED_EVENT_TYPE'
                   )
                   OR (is_reconciled AND reconciliation_status <> 'RECONCILED')
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_factor_reconciliation",),
        ),
        SqlQualityCheck(
            dataset_id="corporate_action_factor_reconciliation",
            table_name="corporate_action_factor_reconciliation",
            check_name="mismatched_corporate_action_factor_steps",
            sql="""
                SELECT count(*)::DOUBLE
                FROM corporate_action_factor_reconciliation
                WHERE reconciliation_status = 'MISMATCH'
            """,
            threshold=0.0,
            comparator="eq",
            required_tables=("corporate_action_factor_reconciliation",),
        ),
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

    offenders: dict[str, list[str]] = {}
    for table_name, specs in resolved_manifest.items():
        is_fact = any(spec.is_pit_column for spec in specs)
        if not is_fact:
            continue
        spec_names = {spec.name for spec in specs}
        missing = sorted(set(PIT_COLUMN_NAMES) - spec_names)
        if missing:
            offenders[table_name] = missing

    observed_value = float(len(offenders))
    passed = observed_value == 0.0
    return QualityResult(
        dataset_id=SCHEMA_CONTRACT_DATASET_ID,
        table_name="schema_contract",
        check_name=PIT_COLUMN_PRESENCE_CHECK_NAME,
        status="passed" if passed else "failed",
        observed_value=observed_value,
        threshold_value=0.0,
        details={"tables_missing_pit_columns": offenders, "checked_at": checked_at.isoformat()},
        severity="critical",
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
    for spec in _check_specs(
        daily_macro_stale_days=daily_macro_stale_days,
        monthly_macro_stale_days=monthly_macro_stale_days,
        valuation_stale_gap_days=valuation_stale_gap_days,
    ):
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
            quality_check(
                store,
                dataset_id=result.dataset_id,
                table_name=result.table_name,
                check_name=result.check_name,
                status=result.status,
                observed_value=result.observed_value,
                threshold_value=result.threshold_value,
                details=result.details,
            )
        results.append(result)

    # PF2-S1 S1-1: catalog-completeness + PIT-column-presence. Both read the S1-2
    # manifest / schema_contract.py's own introspection helpers rather than a static
    # SqlQualityCheck string (the fact/non-fact partition and the catalogued-table set
    # are runtime facts, not compile-time SQL), so they are computed directly and
    # folded into the same result list / record() path as every other check.
    schema_results: list[QualityResult] = []
    if (
        (requested_checks is None and requested_datasets is None)
        or (requested_checks is not None and CATALOG_COMPLETENESS_CHECK_NAME in requested_checks)
        or (requested_datasets is not None and SCHEMA_CONTRACT_DATASET_ID in requested_datasets)
    ):
        schema_results.append(
            catalog_completeness_check(store, checked_at=checked_at, objects=objects)
        )
    if (
        (requested_checks is None and requested_datasets is None)
        or (requested_checks is not None and PIT_COLUMN_PRESENCE_CHECK_NAME in requested_checks)
        or (requested_datasets is not None and SCHEMA_CONTRACT_DATASET_ID in requested_datasets)
    ):
        schema_results.append(pit_column_presence_check(store, checked_at=checked_at))

    for result in schema_results:
        if record:
            quality_check(
                store,
                dataset_id=result.dataset_id,
                table_name=result.table_name,
                check_name=result.check_name,
                status=result.status,
                observed_value=result.observed_value,
                threshold_value=result.threshold_value,
                details=result.details,
            )
        results.append(result)

    return results
