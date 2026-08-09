"""PF-S3 S3-0 concept coverage gates.

The companyfacts re-fetch that materializes the wider concept set is an
operator-run step (for example symbol_source='loaded_facts' with a local
companyfacts_zip). These tests stay fully offline and only verify the committed
projection and loader defaults.
"""
from __future__ import annotations

import csv
from pathlib import Path

from atx_db.fundamental_statements import (
    CONCEPT_MAP_SEED_COLUMNS,
    FUNDAMENTAL_STATEMENT_MAP_ROWS,
    SOURCE_NAME,
    concept_map_projection_rows,
    seed_fundamental_statement_map,
    statement_map_overlay_exception_rows,
    statement_map_unloadable_overlay_rows,
    unexplained_statement_map_overlay_rows,
)
from atx_db.fundamentals import CANONICAL_CONCEPTS, DEFAULT_CONCEPTS, SUPPORTED_FACT_TAXONOMIES
from atx_db.item_registry import read_fundamental_item_seed


CONCEPT_MAP_PATH = Path(__file__).resolve().parents[1] / "seeds" / "concept_map.csv"


def _read_concept_map_seed() -> tuple[tuple[str, str, str, int, str, str], ...]:
    with CONCEPT_MAP_PATH.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        assert tuple(reader.fieldnames or ()) == CONCEPT_MAP_SEED_COLUMNS
        return tuple(
            (
                row["taxonomy"],
                row["concept"],
                row["canonical_metric"],
                int(row["item_id"]),
                row["statement_type"],
                row["industry_template"],
            )
            for row in reader
        )


def _active_all_statement_map_concepts() -> set[tuple[str, str]]:
    supported = set(SUPPORTED_FACT_TAXONOMIES)
    return {
        (row.taxonomy, row.concept)
        for row in FUNDAMENTAL_STATEMENT_MAP_ROWS
        if row.industry_template == "ALL"
        and row.taxonomy in supported
        and row.is_active
        and not row.is_derived
        and not row.concept.startswith("__")
    }


def _insert_item(
    store,
    item_id: int,
    canonical_code: str,
    statement: str,
    *,
    is_derived: bool = False,
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_item (
            item_id, canonical_code, statement, section, data_type, unit_type,
            sign_convention, is_derived, definition, citation
        )
        VALUES (?, ?, ?, 'fixture', 'duration', 'monetary', 'positive', ?, 'fixture', 'fixture')
        """,
        [item_id, canonical_code, statement, is_derived],
    )


def _insert_concept_catalog(
    store,
    taxonomy: str,
    concept: str,
    *,
    source: str = SOURCE_NAME,
) -> None:
    store.con.execute(
        """
        INSERT INTO xbrl_concept_catalog (
            source, taxonomy, concept, label, description, statement_category,
            units_json, forms_json, fiscal_periods_json,
            first_period_end, last_period_end,
            first_filed_date, last_filed_date,
            first_available_at, last_available_at,
            fact_count, security_count, accession_count, latest_source_loaded_at
        )
        VALUES (
            ?, ?, ?, ?, ?, 'test',
            '["USD"]', '["10-K"]', '["FY"]',
            DATE '2023-12-31', DATE '2023-12-31',
            DATE '2024-02-15', DATE '2024-02-15',
            TIMESTAMP '2024-02-15 22:00:00', TIMESTAMP '2024-02-15 22:00:00',
            1, 1, 1, TIMESTAMP '2024-02-15 22:00:00'
        )
        """,
        [source, taxonomy, concept, concept, concept],
    )


def _insert_xbrl_metric(store, security_id: str) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_xbrl_metric (
            metric_id, source, security_id, symbol, cik, canonical_metric, concept,
            taxonomy, unit, period_type, period_start, period_end, fiscal_year,
            fiscal_period, accession_number, value, raw_value, revision_seq,
            is_latest_revision, as_of_date, available_at, run_id
        )
        VALUES (
            ?, 'fixture', ?, ?, '0000000000', 'revenue', 'Revenues',
            'us-gaap', 'USD', 'duration', DATE '2023-01-01', DATE '2023-12-31',
            2023, 'FY', ?, 10.0, '10.0', 1, true, DATE '2023-12-31',
            TIMESTAMP '2024-02-15 22:00:00', 'fixture'
        )
        """,
        [f"xbrl-{security_id}", security_id, security_id, f"acc-{security_id}"],
    )


def _insert_ratio(store, security_id: str) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_ratios (
            ratio_id, source, upstream_source, security_id, symbol, cik, ratio_code,
            ratio_category, ratio_kind, basis, unit, period_start, period_end,
            fiscal_year, fiscal_period, value, numerator_code, numerator_value,
            denominator_code, denominator_value, is_meaningful, is_latest_revision,
            as_of_date, available_at, input_codes_json, run_id
        )
        VALUES (
            ?, 'fixture', 'fixture', ?, ?, '0000000000', 'net_profit_margin',
            'profitability', 'ratio', 'ttm', 'ratio', DATE '2023-01-01',
            DATE '2023-12-31', 2023, 'FY', 0.1, 'net_income', 1.0,
            'revenue', 10.0, true, true, DATE '2023-12-31',
            TIMESTAMP '2024-02-15 22:00:00', '["net_income","revenue"]', 'fixture'
        )
        """,
        [f"ratio-{security_id}", security_id, security_id],
    )


def test_default_concepts_cover_active_all_statement_map_concepts():
    default_concepts = set(DEFAULT_CONCEPTS)
    missing = {
        f"{taxonomy}:{concept}"
        for taxonomy, concept in _active_all_statement_map_concepts()
        if concept not in default_concepts
    }
    assert not missing, f"active ALL-template concepts missing from DEFAULT_CONCEPTS: {sorted(missing)}"


def test_default_concepts_match_reviewable_concept_map_projection():
    rows = _read_concept_map_seed()
    seed_concepts = tuple(sorted({row[1] for row in rows}))
    assert DEFAULT_CONCEPTS == CANONICAL_CONCEPTS
    assert DEFAULT_CONCEPTS == seed_concepts


def test_concept_map_csv_round_trips_generated_projection():
    rows = _read_concept_map_seed()
    assert rows == concept_map_projection_rows()
    keys = [(row[0], row[1], row[4], row[5]) for row in rows]
    assert len(keys) == len(set(keys))


def test_concept_map_rows_match_statement_map_and_registry_item_ids():
    rows = _read_concept_map_seed()
    map_by_key = {
        (row.taxonomy, row.concept, row.industry_template): row
        for row in FUNDAMENTAL_STATEMENT_MAP_ROWS
    }
    registry_item_ids = {row.item_id for row in read_fundamental_item_seed()}

    for taxonomy, concept, canonical_metric, item_id, statement_type, industry_template in rows:
        statement_row = map_by_key[(taxonomy, concept, industry_template)]
        assert statement_row.is_active
        assert not statement_row.is_derived
        assert canonical_metric == statement_row.canonical_metric
        assert item_id == statement_row.item_id
        assert statement_type == statement_row.statement_type
        assert item_id in registry_item_ids


def test_ifrs_remains_excluded_from_loader_and_seed_projection():
    rows = _read_concept_map_seed()
    assert "ifrs-full" not in SUPPORTED_FACT_TAXONOMIES
    assert {row[0] for row in rows} <= set(SUPPORTED_FACT_TAXONOMIES)
    assert all(row[0] != "ifrs-full" for row in rows)


def test_s3_1_fetch_set_has_zero_loaded_concepts_without_statement_map(tmp_store):
    from atx_db.quality import run_warehouse_quality_checks

    seed_fundamental_statement_map(tmp_store)
    for taxonomy, concept in sorted({(row[0], row[1]) for row in _read_concept_map_seed()}):
        _insert_concept_catalog(tmp_store, taxonomy, concept)

    results = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("loaded_xbrl_concepts_without_statement_map",),
        )
    }
    unmapped = results["loaded_xbrl_concepts_without_statement_map"]
    assert unmapped.status == "passed"
    assert unmapped.observed_value == 0.0


def test_s3_1_overlay_exceptions_are_exactly_explained_allowlist():
    exception_rows = statement_map_overlay_exception_rows()
    exception_keys = {
        (row.taxonomy, row.concept, row.industry_template, row.item_id)
        for row in exception_rows
    }
    unloadable_keys = {
        (row.taxonomy, row.concept, row.industry_template, int(row.item_id))
        for row in statement_map_unloadable_overlay_rows()
    }

    assert unloadable_keys == exception_keys
    assert not unexplained_statement_map_overlay_rows()
    assert all(row.reason.strip() for row in exception_rows)

    promoted_item_ids = {1503, 1504, 1505, 1506, 1509, 1601, 1602, 1603, 1605, 1608}
    assert promoted_item_ids.isdisjoint({row.item_id for row in exception_rows})


def test_s3_1_verified_overlay_rows_are_active_with_expected_metadata():
    map_by_key = {
        (row.industry_template, row.taxonomy, row.concept): row
        for row in FUNDAMENTAL_STATEMENT_MAP_ROWS
    }
    expected = {
        ("BK", "us-gaap", "InterestAndDividendIncomeOperating"): (1503, "bank_statement", "interest_income_bank", False),
        ("BK", "us-gaap", "ProvisionForLoanAndLeaseLosses"): (1505, "bank_statement", "loan_loss_provision", False),
        ("BK", "us-gaap", "FinancingReceivableAllowanceForCreditLosses"): (1506, "bank_statement", "allowance_loan_lease_losses", False),
        ("BK", "us-gaap", "LoansAndLeasesReceivableNetReportedAmount"): (1509, "bank_statement", "total_loans", False),
        ("IS", "us-gaap", "PremiumsEarnedNet"): (1601, "insurance_statement", "premiums_earned", False),
        ("IS", "us-gaap", "PremiumsWrittenNet"): (1602, "insurance_statement", "premiums_written", False),
        (
            "IS",
            "us-gaap",
            "SupplementalInformationForPropertyCasualtyInsuranceUnderwritersReservesForUnpaidClaimsAndClaimsAdjustmentExpense",
        ): (1603, "insurance_statement", "loss_reserves", False),
        ("IS", "us-gaap", "LiabilityForUnpaidClaimsAndClaimsAdjustmentExpenseNet"): (
            1605,
            "insurance_statement",
            "unpaid_claim_liability",
            False,
        ),
        ("IS", "us-gaap", "__DERIVED__combined_ratio"): (
            1608,
            "insurance_statement",
            "combined_ratio",
            True,
        ),
    }

    for key, (item_id, statement_type, canonical_metric, is_derived) in expected.items():
        row = map_by_key[key]
        assert row.is_active
        assert row.item_id == item_id
        assert row.statement_type == statement_type
        assert row.canonical_metric == canonical_metric
        assert row.is_derived is is_derived


def test_s3_3_migration_0069_catalogs_reports_and_is_idempotent(tmp_store):
    from atx_db.migrations import _fundamental_concept_coverage_reports

    versions = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
        ).fetchall()
    }
    assert 69 in versions

    expected_views = {
        "fundamental_statement_overlay_allowlist",
        "fundamental_concept_coverage_report",
        "fundamental_unmapped_concept_report",
        "fundamental_xbrl_metric_ratio_universe_gap",
    }
    views = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT view_name FROM duckdb_views() WHERE schema_name = 'main'"
        ).fetchall()
    }
    assert expected_views <= views

    cataloged = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT table_name FROM table_catalog WHERE table_name = ANY(?)",
            [sorted(expected_views)],
        ).fetchall()
    }
    assert cataloged == expected_views

    field_counts = dict(
        tmp_store.con.execute(
            """
            SELECT table_name, count(*)
            FROM field_catalog
            WHERE table_name = ANY(?)
            GROUP BY 1
            """,
            [sorted(expected_views)],
        ).fetchall()
    )
    assert set(field_counts) == expected_views
    assert all(count > 0 for count in field_counts.values())

    _fundamental_concept_coverage_reports(tmp_store.con)
    rows_after_rerun = tmp_store.con.execute(
        "SELECT count(*) FROM fundamental_xbrl_metric_ratio_universe_gap"
    ).fetchone()[0]
    assert rows_after_rerun == 1


def test_s3_3_coverage_report_breaks_out_percent_by_statement_and_template(tmp_store):
    seed_fundamental_statement_map(tmp_store)
    _insert_item(tmp_store, 1001, "revenue", "income")
    _insert_item(tmp_store, 1008, "rd_expense", "income")
    _insert_item(tmp_store, 1503, "interest_income_total", "bank")
    _insert_concept_catalog(tmp_store, "us-gaap", "RevenueFromContractWithCustomerExcludingAssessedTax")
    _insert_concept_catalog(tmp_store, "us-gaap", "InterestAndDividendIncomeOperating")

    rows = {
        (row[0], row[1]): row
        for row in tmp_store.con.execute(
            """
            SELECT
                statement_type,
                industry_template,
                canonical_item_count,
                mapped_item_count,
                extracted_item_count,
                mapped_item_pct,
                extracted_item_pct
            FROM fundamental_concept_coverage_report
            """
        ).fetchall()
    }

    assert rows[("income_statement", "ALL")][2:] == (2, 2, 1, 100.0, 50.0)
    assert rows[("bank_statement", "BK")][2:] == (1, 1, 1, 100.0, 100.0)
    assert rows[("TOTAL", "ALL")][2:] == (3, 3, 2, 100.0, 66.666667)


def test_s3_3_unmapped_report_and_quality_gate_detect_future_gaps(tmp_store):
    from atx_db.quality import run_warehouse_quality_checks

    seed_fundamental_statement_map(tmp_store)
    _insert_item(tmp_store, 1001, "revenue", "income")
    _insert_concept_catalog(tmp_store, "us-gaap", "RevenueFromContractWithCustomerExcludingAssessedTax")

    clean = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("fundamental_unmapped_concept_report_empty",),
        )
    }
    assert clean["fundamental_unmapped_concept_report_empty"].status == "passed"

    _insert_item(tmp_store, 1999, "future_gap", "income")
    _insert_concept_catalog(tmp_store, "us-gaap", "FutureUnmappedConcept")

    rows = tmp_store.con.execute(
        """
        SELECT gap_type, taxonomy, concept, item_id, canonical_code
        FROM fundamental_unmapped_concept_report
        ORDER BY gap_type, item_id NULLS LAST, concept NULLS LAST
        """
    ).fetchall()
    assert (
        "fetched_concept_without_active_map",
        "us-gaap",
        "FutureUnmappedConcept",
        None,
        None,
    ) in rows
    assert (
        "item_without_active_concept",
        None,
        None,
        1999,
        "future_gap",
    ) in rows

    failed = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("fundamental_unmapped_concept_report_empty",),
        )
    }
    gate = failed["fundamental_unmapped_concept_report_empty"]
    assert gate.status == "failed"
    assert gate.observed_value == 2.0
    assert len(gate.details["rows"]) == 2


def test_s3_3_overlay_allowlist_is_excluded_from_unmapped_gate(tmp_store):
    from atx_db.quality import run_warehouse_quality_checks

    seed_fundamental_statement_map(tmp_store)
    _insert_item(tmp_store, 1507, "non_performing_loans", "bank")
    _insert_concept_catalog(tmp_store, "vendor-only", "__VENDOR_ONLY__nonperforming_loans")

    allowlist = tmp_store.con.execute(
        """
        SELECT taxonomy, concept, industry_template, item_id, reason
        FROM fundamental_statement_overlay_allowlist
        WHERE taxonomy = 'vendor-only'
          AND concept = '__VENDOR_ONLY__nonperforming_loans'
          AND industry_template = 'BK'
          AND item_id = 1507
        """
    ).fetchone()
    assert allowlist is not None
    assert allowlist[4]
    assert tmp_store.con.execute(
        "SELECT count(*) FROM fundamental_unmapped_concept_report"
    ).fetchone()[0] == 0

    results = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("fundamental_unmapped_concept_report_empty",),
        )
    }
    assert results["fundamental_unmapped_concept_report_empty"].status == "passed"


def test_s3_3_universe_gap_report_and_quality_check(tmp_store):
    from atx_db.quality import run_warehouse_quality_checks

    _insert_xbrl_metric(tmp_store, "SEC-1")
    _insert_xbrl_metric(tmp_store, "SEC-2")
    _insert_ratio(tmp_store, "SEC-1")

    row = tmp_store.con.execute(
        """
        SELECT
            xbrl_metric_security_count,
            ratio_security_count,
            ratio_minus_xbrl_security_count,
            xbrl_covers_ratio_universe
        FROM fundamental_xbrl_metric_ratio_universe_gap
        """
    ).fetchone()
    assert row == (2, 1, 0, True)

    results = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("xbrl_metric_covers_fundamental_ratio_universe",),
        )
    }
    assert results["xbrl_metric_covers_fundamental_ratio_universe"].status == "passed"

    _insert_ratio(tmp_store, "SEC-3")
    gap = tmp_store.con.execute(
        """
        SELECT
            xbrl_metric_security_count,
            ratio_security_count,
            ratio_minus_xbrl_security_count,
            xbrl_covers_ratio_universe,
            missing_ratio_security_ids_sample
        FROM fundamental_xbrl_metric_ratio_universe_gap
        """
    ).fetchone()
    assert gap == (2, 2, 1, False, "SEC-3")

    failed = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("xbrl_metric_covers_fundamental_ratio_universe",),
        )
    }
    universe = failed["xbrl_metric_covers_fundamental_ratio_universe"]
    assert universe.status == "failed"
    assert universe.observed_value == 1.0
