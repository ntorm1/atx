"""PF-S3 S3-0 concept coverage gates.

The companyfacts re-fetch that materializes the wider concept set is an
operator-run step (for example symbol_source='loaded_facts' with a local
companyfacts_zip). These tests stay fully offline and only verify the committed
projection and loader defaults.
"""
from __future__ import annotations

import csv
from pathlib import Path

from db.fundamental_statements import (
    CONCEPT_MAP_SEED_COLUMNS,
    FUNDAMENTAL_STATEMENT_MAP_ROWS,
    SOURCE_NAME,
    concept_map_projection_rows,
    seed_fundamental_statement_map,
    statement_map_overlay_exception_rows,
    statement_map_unloadable_overlay_rows,
    unexplained_statement_map_overlay_rows,
)
from db.fundamentals import CANONICAL_CONCEPTS, DEFAULT_CONCEPTS, SUPPORTED_FACT_TAXONOMIES
from db.item_registry import read_fundamental_item_seed


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
    from db.quality import run_warehouse_quality_checks

    seed_fundamental_statement_map(tmp_store)
    for taxonomy, concept in sorted({(row[0], row[1]) for row in _read_concept_map_seed()}):
        tmp_store.con.execute(
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
            [SOURCE_NAME, taxonomy, concept, concept, concept],
        )

    results = {r.check_name: r for r in run_warehouse_quality_checks(tmp_store, record=False)}
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
