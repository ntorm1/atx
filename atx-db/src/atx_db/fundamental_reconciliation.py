"""Governed accounting-identity rules for standardized fundamentals."""

from __future__ import annotations

import datetime as dt
import json
import uuid
from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check

SOURCE_NAME = "fundamental_reconciliation_v1"
DQC_0004_URL = "https://xbrl.us/data-rule/dqc_0004/"
SEC_XBRL_GUIDE_URL = "https://www.sec.gov/files/edgar/filer-information/specifications/xbrl-guide-2026-01-16.pdf"


@dataclass(frozen=True)
class ReconciliationRule:
    rule_id: str
    label: str
    statement_type: str
    industry_template: str
    basis: str
    unit_type: str
    tolerance_absolute: float
    tolerance_relative: float
    citation: str
    description: str
    mismatch_severity: str = "error"
    rule_version: str = "1.0.0"
    is_active: bool = True
    valid_from: dt.date = dt.date(1900, 1, 1)
    valid_to: dt.date | None = None


@dataclass(frozen=True)
class ReconciliationTerm:
    rule_id: str
    term_position: int
    term_role: str
    item_id: int
    weight: float
    is_required: bool = True


@dataclass(frozen=True)
class FundamentalReconciliationRefreshOptions:
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


@dataclass(frozen=True)
class FundamentalReconciliationRefreshResult:
    build_id: str
    row_count: int
    max_available_at: dt.datetime | None
    run_id: str


def _fetch_count_watermark(
    store: DuckDBStore,
    query: str,
) -> tuple[int, dt.datetime | None]:
    row = store.con.execute(query).fetchone()
    if row is None:
        raise RuntimeError("reconciliation serving parity query returned no row")
    return int(row[0]), row[1]


def _duration_rules(
    code: str,
    label: str,
    statement_type: str,
    industry_template: str,
    citation: str,
    description: str,
    *,
    unit_type: str = "monetary",
    tolerance_absolute: float = 1.0,
    tolerance_relative: float = 0.000001,
) -> tuple[ReconciliationRule, ...]:
    return tuple(
        ReconciliationRule(
            rule_id=f"{code}_{basis}",
            label=label,
            statement_type=statement_type,
            industry_template=industry_template,
            basis=basis,
            unit_type=unit_type,
            tolerance_absolute=tolerance_absolute,
            tolerance_relative=tolerance_relative,
            citation=citation,
            description=description,
        )
        for basis in ("annual", "quarterly", "ttm")
    )


RECONCILIATION_RULES: tuple[ReconciliationRule, ...] = (
    ReconciliationRule(
        "assets_equal_total_liabilities_equity_instant",
        "Assets equal total liabilities and equity",
        "balance_sheet",
        "ALL",
        "instant",
        "monetary",
        1.0,
        0.000001,
        DQC_0004_URL,
        "Total assets should equal the reported total-liabilities-and-equity line.",
    ),
    ReconciliationRule(
        "total_liabilities_equity_components_instant",
        "Total liabilities and equity components",
        "balance_sheet",
        "ALL",
        "instant",
        "monetary",
        1.0,
        0.000001,
        DQC_0004_URL,
        "Reported total liabilities and equity should equal liabilities plus equity including noncontrolling interests and optional temporary equity.",
    ),
    ReconciliationRule(
        "assets_equal_liabilities_stockholders_equity_instant",
        "DQC 0004 assets, liabilities, and stockholders equity subset",
        "balance_sheet",
        "ALL",
        "instant",
        "monetary",
        1.0,
        0.000001,
        DQC_0004_URL,
        "Same-consolidation-scope DQC 0004 subset used when all three canonical inputs exist.",
        mismatch_severity="diagnostic",
    ),
    ReconciliationRule(
        "total_debt_components_instant",
        "Total debt components",
        "balance_sheet",
        "ALL",
        "instant",
        "monetary",
        1.0,
        0.000001,
        SEC_XBRL_GUIDE_URL,
        "Total debt should equal sign-normalized short-term plus long-term debt.",
    ),
    *_duration_rules(
        "gross_profit_components",
        "Gross profit components",
        "income_statement",
        "ALL",
        SEC_XBRL_GUIDE_URL,
        "Gross profit should equal revenue less sign-normalized cost of revenue.",
    ),
    *_duration_rules(
        "free_cash_flow_components",
        "Free cash flow components",
        "cash_flow",
        "ALL",
        SEC_XBRL_GUIDE_URL,
        "Free cash flow should equal operating cash flow plus sign-normalized capital expenditures.",
    ),
    *_duration_rules(
        "net_change_in_debt_components",
        "Net change in debt components",
        "cash_flow",
        "ALL",
        SEC_XBRL_GUIDE_URL,
        "Net change in debt should equal issuance plus sign-normalized repayments.",
    ),
    *_duration_rules(
        "bank_net_interest_income_components",
        "Bank net interest income components",
        "bank_template",
        "BK",
        SEC_XBRL_GUIDE_URL,
        "Net interest income should equal interest income plus sign-normalized interest expense.",
    ),
    *_duration_rules(
        "insurance_combined_ratio_components",
        "Insurance combined ratio components",
        "insurance_template",
        "IS",
        SEC_XBRL_GUIDE_URL,
        "Combined ratio should equal loss ratio plus expense ratio.",
        unit_type="ratio",
        tolerance_absolute=0.01,
        tolerance_relative=0.0001,
    ),
)


def _three_term_rule(
    rule_id: str,
    lhs: int,
    rhs_one: int,
    rhs_two: int,
    *,
    rhs_two_weight: float = 1.0,
) -> tuple[ReconciliationTerm, ...]:
    return (
        ReconciliationTerm(rule_id, 1, "lhs", lhs, 1.0),
        ReconciliationTerm(rule_id, 2, "rhs", rhs_one, 1.0),
        ReconciliationTerm(rule_id, 3, "rhs", rhs_two, rhs_two_weight),
    )


def _two_term_rule(rule_id: str, lhs: int, rhs: int) -> tuple[ReconciliationTerm, ...]:
    return (
        ReconciliationTerm(rule_id, 1, "lhs", lhs, 1.0),
        ReconciliationTerm(rule_id, 2, "rhs", rhs, 1.0),
    )


def _total_liabilities_equity_terms() -> tuple[ReconciliationTerm, ...]:
    """DQC 0004 rule 9283, where temporary equity is optional and zero-filled."""

    return (
        ReconciliationTerm("total_liabilities_equity_components_instant", 1, "lhs", 1223, 1.0),
        ReconciliationTerm("total_liabilities_equity_components_instant", 2, "rhs", 1201, 1.0),
        ReconciliationTerm("total_liabilities_equity_components_instant", 3, "rhs", 1222, 1.0),
        ReconciliationTerm(
            "total_liabilities_equity_components_instant",
            4,
            "rhs",
            1224,
            1.0,
            is_required=False,
        ),
    )


RECONCILIATION_TERMS: tuple[ReconciliationTerm, ...] = (
    *_two_term_rule("assets_equal_total_liabilities_equity_instant", 1101, 1223),
    *_total_liabilities_equity_terms(),
    *_three_term_rule("assets_equal_liabilities_stockholders_equity_instant", 1101, 1201, 1221),
    *_three_term_rule("total_debt_components_instant", 1208, 1205, 1207),
    *(
        term
        for basis in ("annual", "quarterly", "ttm")
        for term in _three_term_rule(
            f"gross_profit_components_{basis}",
            1004,
            1001,
            1003,
            rhs_two_weight=-1.0,
        )
    ),
    *(
        term
        for basis in ("annual", "quarterly", "ttm")
        for term in _three_term_rule(f"free_cash_flow_components_{basis}", 1325, 1301, 1305)
    ),
    *(
        term
        for basis in ("annual", "quarterly", "ttm")
        for term in _three_term_rule(f"net_change_in_debt_components_{basis}", 1315, 1313, 1314)
    ),
    *(
        term
        for basis in ("annual", "quarterly", "ttm")
        for term in _three_term_rule(f"bank_net_interest_income_components_{basis}", 1501, 1503, 1504)
    ),
    *(
        term
        for basis in ("annual", "quarterly", "ttm")
        for term in _three_term_rule(f"insurance_combined_ratio_components_{basis}", 1608, 1606, 1607)
    ),
)


def seed_fundamental_reconciliation_rules(store: DuckDBStore) -> int:
    """Upsert the closed accounting-identity rule and term registry."""

    with store.transaction():
        store.con.executemany(
            """
            INSERT OR REPLACE INTO fundamental_reconciliation_rule (
                rule_id,rule_version,label,statement_type,industry_template,basis,
                unit_type,tolerance_absolute,tolerance_relative,mismatch_severity,
                citation,description,is_active,valid_from,valid_to,updated_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,now())
            """,
            [
                (
                    rule.rule_id,
                    rule.rule_version,
                    rule.label,
                    rule.statement_type,
                    rule.industry_template,
                    rule.basis,
                    rule.unit_type,
                    rule.tolerance_absolute,
                    rule.tolerance_relative,
                    rule.mismatch_severity,
                    rule.citation,
                    rule.description,
                    rule.is_active,
                    rule.valid_from,
                    rule.valid_to,
                )
                for rule in RECONCILIATION_RULES
            ],
        )
        store.con.executemany(
            """
            INSERT OR REPLACE INTO fundamental_reconciliation_rule_term (
                rule_id,term_position,term_role,item_id,weight,is_required,updated_at
            ) VALUES (?,?,?,?,?,?,now())
            """,
            [
                (
                    term.rule_id,
                    term.term_position,
                    term.term_role,
                    term.item_id,
                    term.weight,
                    term.is_required,
                )
                for term in RECONCILIATION_TERMS
            ],
        )
    return len(RECONCILIATION_RULES)


def refresh_fundamental_reconciliation_serving(
    store: DuckDBStore,
    options: FundamentalReconciliationRefreshOptions | None = None,
) -> FundamentalReconciliationRefreshResult:
    """Atomically publish the contextual reconciliation view into its indexed API table."""

    options = options or FundamentalReconciliationRefreshOptions()
    store.initialize()
    seed_fundamental_reconciliation_rules(store)
    run_id = options.run_id or f"fundamental-reconciliation-{uuid.uuid4()}"
    build_id = str(uuid.uuid4())
    symbols = tuple(sorted({str(symbol).strip().upper() for symbol in options.symbols or () if symbol}))
    scope_json = json.dumps(
        {"symbols": list(symbols) if symbols else None},
        separators=(",", ":"),
        sort_keys=True,
    )
    store.con.execute(
        """
        INSERT INTO fundamental_reconciliation_builds (
            build_id,status,scope_json,started_at,run_id
        ) VALUES (?,'running',?,now(),?)
        """,
        [build_id, scope_json, run_id],
    )
    try:
        if symbols:
            store.con.execute(
                """
                CREATE OR REPLACE TEMP TABLE fundamental_reconciliation_symbol_scope (
                    symbol VARCHAR PRIMARY KEY
                )
                """
            )
            store.con.executemany(
                "INSERT INTO fundamental_reconciliation_symbol_scope VALUES (?)",
                [(symbol,) for symbol in symbols],
            )
            store.con.execute(
                """
                CREATE OR REPLACE TEMP TABLE fundamental_reconciliation_security_scope AS
                SELECT DISTINCT security_id
                FROM (
                    SELECT ticker AS symbol,security_id FROM sec_company_tickers
                    UNION ALL
                    SELECT primary_symbol AS symbol,security_id FROM securities
                    UNION ALL
                    SELECT symbol,security_id FROM fundamental_standardized
                ) candidates
                JOIN fundamental_reconciliation_symbol_scope requested
                  ON upper(trim(candidates.symbol))=requested.symbol
                WHERE candidates.security_id IS NOT NULL
                """
            )
            where_clause = "WHERE security_id IN (SELECT security_id FROM fundamental_reconciliation_security_scope)"
        else:
            where_clause = ""
        store.con.execute(
            f"""
            CREATE OR REPLACE TEMP TABLE fundamental_reconciliation_serving_next AS
            SELECT *
            FROM v_fundamental_reconciliation_contextual
            {where_clause}
            """
        )
        source_row_count, source_max_available_at = _fetch_count_watermark(
            store,
            """
            SELECT count(*),max(available_at)
            FROM fundamental_reconciliation_serving_next
            """,
        )
        with store.transaction():
            if symbols:
                store.con.execute(
                    """
                    DELETE FROM fundamental_reconciliation_serving
                    WHERE security_id IN (
                        SELECT security_id FROM fundamental_reconciliation_security_scope
                    )
                    """
                )
            else:
                store.con.execute("DELETE FROM fundamental_reconciliation_serving")
            store.con.execute(
                """
                INSERT INTO fundamental_reconciliation_serving
                SELECT * FROM fundamental_reconciliation_serving_next
                """
            )
            if symbols:
                serving_row_count, serving_max_available_at = _fetch_count_watermark(
                    store,
                    """
                    SELECT count(*),max(available_at)
                    FROM fundamental_reconciliation_serving
                    WHERE security_id IN (
                        SELECT security_id FROM fundamental_reconciliation_security_scope
                    )
                    """,
                )
            else:
                serving_row_count, serving_max_available_at = _fetch_count_watermark(
                    store,
                    """
                    SELECT count(*),max(available_at)
                    FROM fundamental_reconciliation_serving
                    """,
                )
            if (serving_row_count, serving_max_available_at) != (
                source_row_count,
                source_max_available_at,
            ):
                raise RuntimeError(
                    "reconciliation serving parity failed: "
                    f"source=({source_row_count},{source_max_available_at}) "
                    f"serving=({serving_row_count},{serving_max_available_at})"
                )
            store.con.execute(
                """
                UPDATE fundamental_reconciliation_builds
                SET status='completed',source_row_count=?,serving_row_count=?,
                    source_max_available_at=?,serving_max_available_at=?,
                    completed_at=now(),source_loaded_at=now()
                WHERE build_id=?
                """,
                [
                    source_row_count,
                    serving_row_count,
                    source_max_available_at,
                    serving_max_available_at,
                    build_id,
                ],
            )
        return FundamentalReconciliationRefreshResult(
            build_id=build_id,
            row_count=int(serving_row_count),
            max_available_at=serving_max_available_at,
            run_id=run_id,
        )
    except Exception as exc:
        store.con.execute(
            """
            UPDATE fundamental_reconciliation_builds
            SET status='failed',error_message=?,completed_at=now(),source_loaded_at=now()
            WHERE build_id=?
            """,
            [str(exc)[:2000], build_id],
        )
        raise
    finally:
        store.con.execute("DROP TABLE IF EXISTS fundamental_reconciliation_serving_next")
        store.con.execute("DROP TABLE IF EXISTS fundamental_reconciliation_security_scope")
        store.con.execute("DROP TABLE IF EXISTS fundamental_reconciliation_symbol_scope")


class FundamentalReconciliationDataset(Dataset):
    """Orchestrated materialization of the public reconciliation serving surface."""

    dataset_id = "fundamental_reconciliation"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: FundamentalReconciliationRefreshOptions,
    ) -> DatasetLoadResult:
        result = refresh_fundamental_reconciliation_serving(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fundamental_reconciliation_serving",
            check_name="rows_materialized",
            status="passed" if result.row_count > 0 else "warning",
            observed_value=float(result.row_count),
            threshold_value=1.0,
            details={
                "build_id": result.build_id,
                "max_available_at": result.max_available_at,
                "symbols": options.symbols,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=result.row_count,
            source=self.source_name,
            run_id=result.run_id,
            details={
                "build_id": result.build_id,
                "max_available_at": result.max_available_at,
            },
        )
