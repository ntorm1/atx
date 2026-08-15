from __future__ import annotations

import datetime as dt
import json

import pandas as pd

from atx_db.fundamental_ratios import FundamentalRatiosOptions, refresh_fundamental_ratios
from atx_db.quality import run_warehouse_quality_checks
from atx_db.standardization import (
    SourceAlias,
    StandardizationRule,
    compute_standardization_exceptions,
    compute_standardized_rows,
    read_standardization_rules,
    refresh_fundamental_standardized,
)


def _rule(
    item_id: int,
    *,
    basis: str = "annual",
    combination_rule: str = "coalesce_priority",
    source_item_ids: tuple[int, ...] = (),
    canonical_code: str | None = None,
) -> StandardizationRule:
    return StandardizationRule(
        rule_id=f"test_{basis}_{item_id}",
        item_id=item_id,
        canonical_code=canonical_code or f"item_{item_id}",
        basis=basis,
        source_aliases=(SourceAlias("us-gaap", f"Concept{item_id}", 10),),
        source_item_ids=source_item_ids,
        combination_rule=combination_rule,
        sign_rule="statement_normalized",
        scale_rule="identity",
        missing_policy="skip",
        is_active=True,
        valid_from=dt.date(1900, 1, 1),
        valid_to=None,
    )


def _candidate(
    item_id: int | None,
    value,
    *,
    security_id: str = "SEC-CIK-0000000001",
    basis: str = "annual",
    concept: str = "Concept",
    taxonomy: str = "us-gaap",
    rank: int = 10,
    available_at: dt.datetime = dt.datetime(2026, 2, 1, 22, 0),
) -> dict:
    return {
        "upstream_source": "fixture",
        "source": "fixture",
        "security_id": security_id,
        "symbol": "TST",
        "cik": "1",
        "item_id": item_id,
        "canonical_metric": concept,
        "concept": concept,
        "taxonomy": taxonomy,
        "unit": "USD",
        "unit_type": "monetary",
        "basis": basis,
        "period_start": dt.date(2025, 1, 1) if basis != "instant" else None,
        "period_end": dt.date(2025, 12, 31),
        "fiscal_year": 2025,
        "fiscal_period": "FY",
        "accession_number": "acc",
        "source_accession": "acc",
        "filed_date": dt.date(2026, 2, 1),
        "value": value,
        "available_at": available_at,
        "input_rank": rank,
    }


def test_standardization_rule_seed_covers_template_items():
    rules = read_standardization_rules()
    by_basis = {}
    for rule in rules:
        by_basis.setdefault(rule.basis, set()).add(rule.item_id)

    assert len(rules) == 455
    assert len(by_basis["annual"]) == 130
    assert len(by_basis["quarterly"]) == 130
    assert len(by_basis["ttm"]) == 130
    assert len(by_basis["instant"]) == 65
    revenue = next(rule for rule in rules if rule.rule_id == "std_annual_1001")
    assert revenue.combination_rule == "coalesce_priority"
    assert [alias.alias_code for alias in revenue.source_aliases] == [
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        "Revenues",
        "SalesRevenueNet",
    ]
    total_debt = next(rule for rule in rules if rule.rule_id == "std_instant_1208")
    assert total_debt.combination_rule == "sum"
    assert total_debt.source_item_ids == (1205, 1207)
    temporary_equity = next(rule for rule in rules if rule.rule_id == "std_instant_1224")
    assert [alias.alias_code for alias in temporary_equity.source_aliases] == [
        "TemporaryEquityCarryingAmountIncludingPortionAttributableToNoncontrollingInterests",
        "RedeemableNoncontrollingInterestEquityCarryingAmount",
        "TemporaryEquityCarryingAmount",
        "RedeemablePreferredStockCarryingAmount",
        "TemporaryEquityRedemptionValue",
        "RedeemableNoncontrollingInterestEquityPreferredCarryingAmount",
    ]
    net_debt_issuance = next(rule for rule in rules if rule.rule_id == "std_quarterly_1315")
    assert net_debt_issuance.combination_rule == "sum"
    assert net_debt_issuance.source_item_ids == (1313, 1314)


def test_compute_standardized_rows_closed_dispatch_rules():
    rules = (
        _rule(9001, combination_rule="coalesce_priority"),
        _rule(9002, combination_rule="first_non_null"),
        _rule(9003, combination_rule="identity"),
        _rule(9010, combination_rule="sum", source_item_ids=(9004, 9005)),
        _rule(9011, combination_rule="difference", source_item_ids=(9006, 9007)),
    )
    inputs = pd.DataFrame(
        [
            _candidate(9001, 10.0, concept="FallbackRevenue", rank=20),
            _candidate(9001, 11.0, concept="PreferredRevenue", rank=10),
            _candidate(9002, None, concept="Blank", rank=10),
            _candidate(9002, 12.0, concept="Present", rank=20),
            _candidate(9003, 13.0),
            _candidate(9004, 4.0),
            _candidate(9005, 5.0),
            _candidate(9006, 9.0),
            _candidate(9007, 2.0),
        ]
    )

    out = compute_standardized_rows(inputs, rules=rules)
    values = dict(zip(out["item_id"], out["value"], strict=True))

    assert values[9001] == 11.0
    assert values[9002] == 12.0
    assert values[9003] == 13.0
    assert values[9010] == 9.0
    assert values[9011] == 7.0
    revenue = out.loc[out["item_id"] == 9001].iloc[0]
    assert json.loads(revenue["input_codes_json"]) == ["us-gaap:PreferredRevenue"]


def test_compute_standardization_exceptions_accounts_for_unmapped_inputs():
    inputs = pd.DataFrame(
        [
            _candidate(None, 1.0, concept="CustomNotMapped", taxonomy="acme"),
            _candidate(1001, 2.0, concept="Revenue"),
        ]
    )
    rules = (_rule(1001, canonical_code="revenue"),)

    out = compute_standardization_exceptions(inputs, rules=rules)

    assert len(out) == 1
    assert out.iloc[0]["concept"] == "CustomNotMapped"
    assert out.iloc[0]["reason"] == "unmapped_concept"


def _insert_xbrl_metric(
    store,
    *,
    metric_id: str,
    security_id: str,
    symbol: str,
    canonical_metric: str,
    concept: str,
    taxonomy: str,
    value: float,
    period_end: dt.date,
    available_at: dt.datetime,
    period_type: str = "duration",
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_xbrl_metric (
            metric_id, source, security_id, symbol, cik, canonical_metric, concept,
            taxonomy, unit, period_type, period_start, period_end, accession_number,
            value, is_latest_revision, as_of_date, available_at
        )
        VALUES (?, 'fixture_xbrl', ?, ?, '0000000001', ?, ?, ?, 'USD', ?, ?, ?, ?, ?, true, ?, ?)
        """,
        [
            metric_id,
            security_id,
            symbol,
            canonical_metric,
            concept,
            taxonomy,
            period_type,
            dt.date(period_end.year, 1, 1) if period_type == "duration" else None,
            period_end,
            f"{metric_id}-acc",
            value,
            period_end,
            available_at,
        ],
    )


def _insert_statement_duration(
    store,
    *,
    statement_point_id: str,
    security_id: str,
    value: float,
    period_start: dt.date,
    period_end: dt.date,
    available_at: dt.datetime,
    fiscal_period: str,
    canonical_metric: str = "revenue",
    concept: str = "Revenues",
    statement_type: str = "income_statement",
    statement_section: str = "revenue",
    revision_sequence: int = 1,
    revision_count: int = 1,
    is_latest_revision: bool = True,
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_statement_points (
            statement_point_id,fact_revision_id,revision_group_id,source,security_id,symbol,cik,
            statement_type,statement_section,canonical_metric,canonical_label,taxonomy,concept,
            unit,unit_type,period_type,normal_balance,period_start,period_end,as_of_date,
            available_at,fiscal_year,fiscal_period,form,accession_number,source_accession,
            filed_date,revision_sequence,revision_count,is_latest_revision,is_value_changed,
            raw_value,value,run_id,source_url,source_loaded_at
        ) VALUES (
            ?,?,?, 'sec_companyfacts',?,'REV','0000000006',
            ?,?,?,?,'us-gaap',?,
            'USD','monetary','duration','credit',?,?,?,?,2025,?,'10-Q',?,?,?, ?,?,?,?,
            ?,?,'test','https://data.sec.gov/',?
        )
        """,
        [
            statement_point_id,
            f"fact-{statement_point_id}",
            f"group-{period_start}-{period_end}",
            security_id,
            statement_type,
            statement_section,
            canonical_metric,
            canonical_metric.replace("_", " ").title(),
            concept,
            period_start,
            period_end,
            period_end,
            available_at,
            fiscal_period,
            f"{statement_point_id}-accession",
            f"{statement_point_id}-accession",
            available_at.date(),
            revision_sequence,
            revision_count,
            is_latest_revision,
            revision_sequence > 1,
            value,
            value,
            available_at,
        ],
    )


def test_refresh_routes_custom_extension_and_records_unmapped_exception(tmp_store):
    from atx_db.item_registry import seed_fundamental_item_registry

    sid = "SEC-CIK-0000000001"
    end = dt.date(2025, 12, 31)
    av = dt.datetime(2026, 2, 1, 22, 0)
    seed_fundamental_item_registry(tmp_store)
    tmp_store.con.execute(
        """
        INSERT INTO fundamental_item_vendor_map (item_id, vendor, vendor_field, sign_note)
        VALUES (1001, 'acme-test', 'AcmeRevenueCustom', 'fixture custom extension')
        """
    )
    _insert_xbrl_metric(
        tmp_store,
        metric_id="mapped-custom",
        security_id=sid,
        symbol="TST",
        canonical_metric="custom_revenue",
        concept="AcmeRevenueCustom",
        taxonomy="acme-test",
        value=123.0,
        period_end=end,
        available_at=av,
    )
    _insert_xbrl_metric(
        tmp_store,
        metric_id="unmapped-custom",
        security_id=sid,
        symbol="TST",
        canonical_metric="mystery_metric",
        concept="AcmeMysteryCustom",
        taxonomy="acme-test",
        value=7.0,
        period_end=end,
        available_at=av,
    )

    result = refresh_fundamental_standardized(tmp_store)

    assert 1001 in set(result.standardized["item_id"])
    row = tmp_store.con.execute(
        """
        SELECT item_id, value, input_codes_json
        FROM fundamental_standardized
        WHERE security_id = ? AND item_id = 1001 AND basis = 'annual'
        """,
        [sid],
    ).fetchone()
    assert row[0] == 1001
    assert row[1] == 123.0
    assert "AcmeRevenueCustom" in row[2]
    exc = tmp_store.con.execute(
        "SELECT concept, reason FROM fundamental_standardization_exception WHERE concept = 'AcmeMysteryCustom'"
    ).fetchone()
    assert exc == ("AcmeMysteryCustom", "unmapped_concept")


def test_set_based_refresh_retains_revisions_classifies_quarters_and_writes_manifest(tmp_store):
    sid = "SEC-CIK-0000000006"
    annual_end = dt.date(2025, 12, 31)
    first_available = dt.datetime(2026, 2, 1, 22, 0)
    restated_available = dt.datetime(2026, 3, 1, 22, 0)
    _insert_xbrl_metric(
        tmp_store,
        metric_id="revenue-original",
        security_id=sid,
        symbol="REV",
        canonical_metric="revenue",
        concept="Revenues",
        taxonomy="us-gaap",
        value=100.0,
        period_end=annual_end,
        available_at=first_available,
    )
    _insert_xbrl_metric(
        tmp_store,
        metric_id="revenue-restated",
        security_id=sid,
        symbol="REV",
        canonical_metric="revenue",
        concept="Revenues",
        taxonomy="us-gaap",
        value=110.0,
        period_end=annual_end,
        available_at=restated_available,
    )
    _insert_xbrl_metric(
        tmp_store,
        metric_id="revenue-quarter",
        security_id=sid,
        symbol="REV",
        canonical_metric="revenue",
        concept="Revenues",
        taxonomy="us-gaap",
        value=25.0,
        period_end=dt.date(2026, 3, 31),
        available_at=dt.datetime(2026, 5, 1, 22, 0),
    )

    result = refresh_fundamental_standardized(
        tmp_store,
        options=None,
    )

    revisions = tmp_store.con.execute(
        """
        SELECT value,revision_sequence,revision_count,is_value_changed,
               previous_value,update_type,valid_to,is_latest_revision
        FROM fundamental_standardized
        WHERE security_id=? AND canonical_code='revenue' AND basis='annual'
        ORDER BY available_at
        """,
        [sid],
    ).fetchall()
    assert revisions == [
        (100.0, 1, 2, False, None, "original", restated_available, False),
        (110.0, 2, 2, True, 100.0, "restated", None, True),
    ]
    quarterly = tmp_store.con.execute(
        """
        SELECT value,rule_id,unit,unit_type
        FROM fundamental_standardized
        WHERE security_id=? AND basis='quarterly'
        """,
        [sid],
    ).fetchone()
    assert quarterly == (25.0, "std_quarterly_1001", "USD", "monetary")
    assert tmp_store.con.execute("SELECT count(*) FROM fundamental_item").fetchone()[0] == 235
    assert (
        tmp_store.con.execute(
            """
        SELECT template_item_count
        FROM v_fundamental_standardization_coverage
        WHERE security_id=? AND basis='quarterly'
        """,
            [sid],
        ).fetchone()[0]
        == 130.0
    )
    manifest = tmp_store.con.execute(
        """
        SELECT status,rule_count,input_row_count,standardized_row_count,
               exception_row_count,basis_counts_json
        FROM fundamental_standardization_builds WHERE build_id=?
        """,
        [result.build_id],
    ).fetchone()
    assert manifest[:5] == ("completed", 455, 3, 3, 0)
    assert json.loads(manifest[5]) == {"annual": 2, "quarterly": 1}
    assert result.standardized_row_count == 3
    assert result.rule_set_sha256 is not None


def test_refresh_derives_pit_discrete_q4_and_defers_to_reported_quarter(tmp_store):
    sid = "SEC-CIK-0000000006"
    year_start = dt.date(2025, 1, 1)
    nine_month_end = dt.date(2025, 9, 30)
    year_end = dt.date(2025, 12, 31)
    _insert_statement_duration(
        tmp_store,
        statement_point_id="nine-month-original",
        security_id=sid,
        value=270.0,
        period_start=year_start,
        period_end=nine_month_end,
        available_at=dt.datetime(2025, 11, 1, 22, 0),
        fiscal_period="Q3",
        revision_count=3,
        is_latest_revision=False,
    )
    _insert_statement_duration(
        tmp_store,
        statement_point_id="annual",
        security_id=sid,
        value=400.0,
        period_start=year_start,
        period_end=year_end,
        available_at=dt.datetime(2026, 2, 1, 22, 0),
        fiscal_period="FY",
    )
    _insert_statement_duration(
        tmp_store,
        statement_point_id="nine-month-restated",
        security_id=sid,
        value=280.0,
        period_start=year_start,
        period_end=nine_month_end,
        available_at=dt.datetime(2026, 3, 1, 22, 0),
        fiscal_period="Q3",
        revision_sequence=2,
        revision_count=3,
        is_latest_revision=False,
    )
    _insert_statement_duration(
        tmp_store,
        statement_point_id="reported-q4",
        security_id=sid,
        value=125.0,
        period_start=dt.date(2025, 10, 1),
        period_end=year_end,
        available_at=dt.datetime(2026, 4, 1, 22, 0),
        fiscal_period="Q4",
    )
    _insert_statement_duration(
        tmp_store,
        statement_point_id="nine-month-restated-after-q4",
        security_id=sid,
        value=275.0,
        period_start=year_start,
        period_end=nine_month_end,
        available_at=dt.datetime(2026, 5, 1, 22, 0),
        fiscal_period="Q3",
        revision_sequence=3,
        revision_count=3,
    )

    refresh_fundamental_standardized(tmp_store)

    revisions = tmp_store.con.execute(
        """
        SELECT value,available_at,fiscal_period,upstream_source,combination_rule,
               revision_sequence,revision_count,is_latest_revision,input_codes_json
        FROM fundamental_standardized
        WHERE security_id=? AND canonical_code='revenue' AND basis='quarterly'
          AND period_end=?
        ORDER BY available_at
        """,
        [sid, year_end],
    ).fetchall()
    assert [row[:8] for row in revisions] == [
        (
            130.0,
            dt.datetime(2026, 2, 1, 22, 0),
            "Q4_DERIVED",
            "fundamental_statement_points_derived_quarter",
            "discrete_quarter_difference",
            1,
            3,
            False,
        ),
        (
            120.0,
            dt.datetime(2026, 3, 1, 22, 0),
            "Q4_DERIVED",
            "fundamental_statement_points_derived_quarter",
            "discrete_quarter_difference",
            2,
            3,
            False,
        ),
        (
            125.0,
            dt.datetime(2026, 4, 1, 22, 0),
            "Q4",
            "fundamental_statement_points",
            "coalesce_priority",
            3,
            3,
            True,
        ),
    ]
    assert json.loads(revisions[0][8]) == ["us-gaap:Revenues", "us-gaap:Revenues"]


def test_quarterly_combination_rules_consume_derived_components(tmp_store):
    sid = "SEC-CIK-0000000007"
    year_start = dt.date(2025, 1, 1)
    nine_month_end = dt.date(2025, 9, 30)
    year_end = dt.date(2025, 12, 31)
    for metric, concept, prior_value, annual_value in (
        ("lt_debt_issued", "ProceedsFromIssuanceOfLongTermDebt", 90.0, 120.0),
        ("lt_debt_repaid", "RepaymentsOfLongTermDebt", -50.0, -70.0),
    ):
        _insert_statement_duration(
            tmp_store,
            statement_point_id=f"{metric}-nine-month",
            security_id=sid,
            value=prior_value,
            period_start=year_start,
            period_end=nine_month_end,
            available_at=dt.datetime(2025, 11, 1, 22, 0),
            fiscal_period="Q3",
            canonical_metric=metric,
            concept=concept,
            statement_type="cash_flow",
            statement_section="financing",
        )
        _insert_statement_duration(
            tmp_store,
            statement_point_id=f"{metric}-annual",
            security_id=sid,
            value=annual_value,
            period_start=year_start,
            period_end=year_end,
            available_at=dt.datetime(2026, 2, 1, 22, 0),
            fiscal_period="FY",
            canonical_metric=metric,
            concept=concept,
            statement_type="cash_flow",
            statement_section="financing",
        )

    refresh_fundamental_standardized(tmp_store)

    rows = tmp_store.con.execute(
        """
        SELECT canonical_code,value,combination_rule,input_codes_json
        FROM fundamental_standardized
        WHERE security_id=? AND basis='quarterly' AND period_end=?
        ORDER BY item_id
        """,
        [sid, year_end],
    ).fetchall()
    assert [(row[0], row[1], row[2]) for row in rows] == [
        ("lt_debt_issued", 30.0, "discrete_quarter_difference"),
        ("lt_debt_repaid", -20.0, "discrete_quarter_difference"),
        ("net_change_in_debt", 10.0, "sum"),
    ]
    assert json.loads(rows[-1][3]) == [
        "atx-derived:lt_debt_issued",
        "atx-derived:lt_debt_repaid",
    ]


def _insert_standardized(
    store,
    *,
    security_id: str,
    symbol: str,
    item_id: int,
    canonical_code: str,
    basis: str,
    value: float,
    period_end: dt.date,
    available_at: dt.datetime,
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, upstream_source, security_id, symbol, cik,
            item_id, canonical_code, basis, period_start, period_end, value, unit_type,
            source_accession, filed_date, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, is_latest_revision
        )
        VALUES (?, 'standardized_fixture', 'fixture', ?, ?, '0000000001', ?, ?, ?, ?, ?, ?, 'monetary',
                'std-acc', ?, ?, ?, ?, ?, 'fixture_rule', 'identity', true)
        """,
        [
            f"{security_id}|{item_id}|{basis}|{period_end}",
            security_id,
            symbol,
            item_id,
            canonical_code,
            basis,
            dt.date(period_end.year - 1, 1, 1) if basis != "instant" else None,
            period_end,
            value,
            available_at.date(),
            period_end,
            available_at,
            json.dumps([canonical_code]),
            json.dumps([item_id]),
        ],
    )


def test_ratio_loader_reads_standardized_inputs_without_raw_ttm_anchor(tmp_store):
    sid = "SEC-CIK-0000000002"
    sym = "STD"
    end = dt.date(2025, 12, 31)
    av = dt.datetime(2026, 2, 1, 22, 0)
    for item_id, code, basis, value in (
        (1001, "revenue", "ttm", 400.0),
        (1031, "net_income", "ttm", 100.0),
        (1301, "operating_cash_flow", "ttm", 130.0),
        (1305, "capital_expenditures", "ttm", -30.0),
        (1101, "assets", "instant", 350.0),
        (1201, "liabilities", "instant", 290.0),
        (1221, "stockholders_equity", "instant", 60.0),
    ):
        _insert_standardized(
            tmp_store,
            security_id=sid,
            symbol=sym,
            item_id=item_id,
            canonical_code=code,
            basis=basis,
            value=value,
            period_end=end,
            available_at=av,
        )

    rows = refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions())
    values = dict(
        tmp_store.con.execute(
            "SELECT ratio_code, value FROM fundamental_ratios WHERE security_id = ?",
            [sid],
        ).fetchall()
    )

    assert rows > 0
    assert values["net_profit_margin"] == 0.25
    assert values["return_on_equity"] == 100.0 / 60.0
    assert values["free_cash_flow"] == 100.0


def test_migrations_catalog_and_contract_include_standardization_tables(tmp_store):
    versions = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
        ).fetchall()
    }
    assert {103, 104, 105, 106, 271}.issubset(versions)
    catalogued = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM table_catalog
            WHERE table_name = ANY(?)
            """,
            [
                [
                    "fundamental_standardized",
                    "fundamental_standardization_exception",
                    "v_fundamental_standardization_coverage",
                ]
            ],
        ).fetchall()
    }
    assert catalogued == {
        "fundamental_standardized",
        "fundamental_standardization_exception",
        "v_fundamental_standardization_coverage",
    }
    contract_rows = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM schema_contract
        WHERE table_name = 'fundamental_standardized'
        """
    ).fetchone()[0]
    assert contract_rows > 0
    indexes = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT index_name FROM duckdb_indexes() WHERE table_name = 'fundamental_standardized'"
        ).fetchall()
    }
    assert "idx_fundamental_standardized_item" in indexes


def test_standardization_quality_gates_fire_and_pass(tmp_store):
    sid = "SEC-CIK-0000000003"
    end = dt.date(2025, 12, 31)
    av = dt.datetime(2026, 2, 1, 22, 0)
    tmp_store.con.execute(
        """
        INSERT INTO fundamental_standardization_exception (
            exception_id, source, security_id, symbol, basis, period_end,
            concept, taxonomy, reason, as_of_date, available_at
        )
        VALUES ('all-unmapped', 'quality_fixture', ?, 'BAD', 'annual', ?, 'Mystery', 'acme',
                'unmapped_concept', ?, ?)
        """,
        [sid, end, end, av],
    )
    exception_result = run_warehouse_quality_checks(
        tmp_store,
        check_names=("fundamental_standardization_exception_rate",),
        record=False,
    )[0]
    assert exception_result.status == "failed"

    tmp_store.con.execute("DELETE FROM fundamental_standardization_exception")
    sparse_sid = "SEC-CIK-0000000004"
    _insert_standardized(
        tmp_store,
        security_id=sparse_sid,
        symbol="SPR",
        item_id=1001,
        canonical_code="revenue",
        basis="annual",
        value=1.0,
        period_end=end,
        available_at=av,
    )
    sparse_result = run_warehouse_quality_checks(
        tmp_store,
        check_names=("fundamental_standardization_template_coverage",),
        record=False,
    )[0]
    assert sparse_result.status == "failed"

    tmp_store.con.execute("DELETE FROM fundamental_standardized")
    for offset, item_id in enumerate(range(1001, 1010)):
        _insert_standardized(
            tmp_store,
            security_id="SEC-CIK-0000000005",
            symbol="FULL",
            item_id=item_id,
            canonical_code=f"item_{item_id}",
            basis="annual",
            value=float(offset + 1),
            period_end=end,
            available_at=av,
        )
    coverage_result = run_warehouse_quality_checks(
        tmp_store,
        check_names=("fundamental_standardization_template_coverage",),
        record=False,
    )[0]
    assert coverage_result.status == "passed"
