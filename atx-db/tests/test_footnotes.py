from __future__ import annotations

import datetime as dt

import pandas as pd

from atx_db.footnotes import FootnoteOptions, compute_footnote_rows, refresh_footnotes


SECURITY_ID = "SEC-CIK-0000000008"
CIK = "0000000008"
SYMBOL = "FNT"
ACCESSION = "0000000008-25-000001"
PERIOD_START = dt.date(2024, 1, 1)
PERIOD_END = dt.date(2024, 12, 31)
AVAILABLE_AT = dt.datetime(2025, 2, 20, 18, 0)


def _candidate(concept: str, value: float, axis: str, member: str, fact_id: str) -> dict:
    return {
        "filing_fact_id": fact_id,
        "filing_context_id": f"ctx-{fact_id}",
        "filing_dimension_id": f"dim-{fact_id}",
        "security_id": SECURITY_ID,
        "symbol": SYMBOL,
        "cik": CIK,
        "accession_number": ACCESSION,
        "form": "10-K",
        "filing_date": AVAILABLE_AT.date(),
        "acceptance_datetime": AVAILABLE_AT,
        "primary_document": "fnt-2024.htm",
        "context_id": f"c-{fact_id}",
        "period_type": "duration",
        "period_start": PERIOD_START,
        "period_end": PERIOD_END,
        "instant_date": None,
        "context_element": "segment",
        "member_kind": "explicit",
        "dimension_qname": axis,
        "dimension_taxonomy": "us-gaap",
        "dimension_concept": axis.split(":")[-1],
        "member_qname": member,
        "member_taxonomy": "us-gaap",
        "member_concept": member.split(":")[-1],
        "typed_member_value": None,
        "member_text": member.split(":")[-1].replace("Member", ""),
        "concept": concept,
        "taxonomy": "us-gaap",
        "unit": "USD",
        "value": value,
        "raw_value": str(value),
        "source_url": "https://www.sec.gov/Archives/edgar/data/8/fnt-2024.htm",
        "available_at": AVAILABLE_AT,
        "source_loaded_at": AVAILABLE_AT + dt.timedelta(minutes=5),
    }


def test_compute_footnote_rows_routes_all_families() -> None:
    rows = pd.DataFrame(
        [
            _candidate(
                "DefinedBenefitPlanBenefitObligation",
                100.0,
                "us-gaap:RetirementPlanTypeAxis",
                "us-gaap:PensionPlansDefinedBenefitMember",
                "pbo",
            ),
            _candidate(
                "DeferredTaxAssetsNet",
                20.0,
                "us-gaap:IncomeTaxAuthorityAxis",
                "us-gaap:FederalMember",
                "dta",
            ),
            _candidate(
                "OperatingLeaseLiability",
                30.0,
                "us-gaap:LeaseTypeAxis",
                "us-gaap:OperatingLeasesMember",
                "lease",
            ),
            _candidate(
                "ShareBasedCompensation",
                4.0,
                "us-gaap:AwardTypeAxis",
                "us-gaap:EmployeeStockOptionMember",
                "sbc",
            ),
        ]
    )

    out = compute_footnote_rows(rows, source="fixture")

    assert out["footnote_pension"].iloc[0]["plan_type"] == "pension"
    assert out["footnote_pension"].iloc[0]["line_item"] == "benefit_obligation"
    assert out["footnote_deferred_tax"].iloc[0]["line_item"] == "deferred_tax_assets_net"
    assert out["footnote_lease"].iloc[0]["lease_standard"] == "asc_842"
    assert out["footnote_lease"].iloc[0]["lease_class"] == "OperatingLeases"
    assert out["footnote_sbc"].iloc[0]["award_type"] == "EmployeeStockOption"


def _seed_security(store) -> None:
    store.con.execute(
        "INSERT OR REPLACE INTO securities (security_id, primary_symbol, source) VALUES (?, ?, ?)",
        [SECURITY_ID, SYMBOL, "test"],
    )


def _insert_dimensional_context(store, *, fact_id: str, axis: str, member: str) -> None:
    context_id = f"ctx-{fact_id}"
    store.con.execute(
        """
        INSERT INTO xbrl_filing_contexts (
            filing_context_id, security_id, cik, accession_number, form,
            filing_date, report_date, acceptance_datetime, primary_document,
            context_id, entity_identifier_scheme, entity_identifier,
            period_type, period_start, period_end, instant_date,
            has_segment, has_scenario, explicit_member_count, typed_member_count,
            dimension_count, context_hash, source_url, run_id, source_loaded_at
        )
        VALUES (
            ?, ?, ?, ?, '10-K',
            DATE '2025-02-20', DATE '2024-12-31', TIMESTAMP '2025-02-20 18:00:00', 'fnt-2024.htm',
            ?, 'http://www.sec.gov/CIK', ?,
            'duration', DATE '2024-01-01', DATE '2024-12-31', NULL,
            true, false, 1, 0,
            1, ?, 'https://www.sec.gov/Archives/edgar/data/8/fnt-2024.htm', 'test', TIMESTAMP '2025-02-20 18:05:00'
        )
        """,
        [context_id, SECURITY_ID, CIK, ACCESSION, f"c-{fact_id}", CIK, f"hash-{fact_id}"],
    )
    store.con.execute(
        """
        INSERT INTO xbrl_filing_dimensions (
            filing_dimension_id, filing_context_id, security_id, cik, accession_number,
            form, filing_date, acceptance_datetime, primary_document, context_id,
            context_element, member_kind, dimension_qname, dimension_taxonomy,
            dimension_concept, member_qname, member_taxonomy, member_concept,
            typed_member_value, member_text, ordinal, source_url, run_id, source_loaded_at
        )
        VALUES (
            ?, ?, ?, ?, ?, '10-K', DATE '2025-02-20', TIMESTAMP '2025-02-20 18:00:00',
            'fnt-2024.htm', ?, 'segment', 'explicit', ?, 'us-gaap', ?,
            ?, 'us-gaap', ?, NULL, ?, 0,
            'https://www.sec.gov/Archives/edgar/data/8/fnt-2024.htm', 'test', TIMESTAMP '2025-02-20 18:05:00'
        )
        """,
        [
            f"dim-{fact_id}",
            context_id,
            SECURITY_ID,
            CIK,
            ACCESSION,
            f"c-{fact_id}",
            axis,
            axis.split(":")[-1],
            member,
            member.split(":")[-1],
            member.split(":")[-1].replace("Member", ""),
        ],
    )


def _insert_fact(store, *, fact_id: str, concept: str, value: float) -> None:
    context_id = f"ctx-{fact_id}"
    store.con.execute(
        """
        INSERT INTO xbrl_filing_facts (
            filing_fact_id, filing_context_id, security_id, cik, accession_number,
            form, filing_date, acceptance_datetime, primary_document, fact_ordinal,
            fact_kind, ix_id, qname, taxonomy, concept, context_ref, unit_ref,
            unit_measures_json, unit_numerator_measures_json, unit_denominator_measures_json,
            decimals, precision, scale, sign, format, continued_at, is_hidden,
            raw_value, normalized_value, numeric_value, is_numeric, source_line,
            source_url, run_id, source_loaded_at
        )
        VALUES (
            ?, ?, ?, ?, ?, '10-K', DATE '2025-02-20', TIMESTAMP '2025-02-20 18:00:00',
            'fnt-2024.htm', 1, 'nonFraction', NULL, 'us-gaap:' || ?, 'us-gaap',
            ?, ?, 'USD', '["iso4217:USD"]', '[]', '[]', '0', NULL, NULL, NULL, NULL, NULL, false,
            CAST(? AS VARCHAR), CAST(? AS VARCHAR), ?, true, 1,
            'https://www.sec.gov/Archives/edgar/data/8/fnt-2024.htm', 'test', TIMESTAMP '2025-02-20 18:05:00'
        )
        """,
        [fact_id, context_id, SECURITY_ID, CIK, ACCESSION, concept, concept, f"c-{fact_id}", value, value, value],
    )


def _seed_footnote_fact(store, *, fact_id: str, concept: str, value: float, axis: str, member: str) -> None:
    _insert_dimensional_context(store, fact_id=fact_id, axis=axis, member=member)
    _insert_fact(store, fact_id=fact_id, concept=concept, value=value)


def test_refresh_footnotes_populates_all_subledgers(tmp_store) -> None:
    _seed_security(tmp_store)
    fixtures = [
        (
            "pbo",
            "DefinedBenefitPlanBenefitObligation",
            100.0,
            "us-gaap:RetirementPlanTypeAxis",
            "us-gaap:PensionPlansDefinedBenefitMember",
        ),
        (
            "dta",
            "DeferredTaxAssetsNet",
            20.0,
            "us-gaap:IncomeTaxAuthorityAxis",
            "us-gaap:FederalMember",
        ),
        (
            "lease842",
            "OperatingLeaseLiability",
            30.0,
            "us-gaap:LeaseTypeAxis",
            "us-gaap:OperatingLeasesMember",
        ),
        (
            "leasepre842",
            "OperatingLeasesFutureMinimumPaymentsDueCurrent",
            12.0,
            "us-gaap:LeaseTypeAxis",
            "us-gaap:OperatingLeasesMember",
        ),
        (
            "sbc",
            "EmployeeServiceShareBasedCompensationTaxBenefitFromCompensationExpense",
            3.0,
            "us-gaap:AwardTypeAxis",
            "us-gaap:RestrictedStockUnitsRSUMember",
        ),
    ]
    for fact_id, concept, value, axis, member in fixtures:
        _seed_footnote_fact(tmp_store, fact_id=fact_id, concept=concept, value=value, axis=axis, member=member)

    counts = refresh_footnotes(tmp_store, FootnoteOptions(source="footnote_fixture"))

    assert counts == {
        "footnote_pension_rows": 1,
        "footnote_deferred_tax_rows": 1,
        "footnote_lease_rows": 2,
        "footnote_sbc_rows": 1,
    }
    lease_rows = tmp_store.con.execute(
        """
        SELECT lease_standard, maturity_bucket
        FROM footnote_lease
        WHERE source = 'footnote_fixture'
        ORDER BY lease_standard, maturity_bucket
        """
    ).fetchall()
    assert ("asc_842", None) in lease_rows
    assert ("pre_842", "current") in lease_rows
    sbc = tmp_store.con.execute(
        "SELECT award_type, line_item FROM footnote_sbc WHERE source = 'footnote_fixture'"
    ).fetchone()
    assert sbc == ("RestrictedStockUnitsRSU", "share_based_compensation_tax_benefit")


def test_footnote_migrations_catalog_and_contract(tmp_store) -> None:
    versions = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
        ).fetchall()
    }
    assert 118 in versions
    catalogued = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM table_catalog
            WHERE table_name IN (
                'footnote_pension',
                'footnote_deferred_tax',
                'footnote_lease',
                'footnote_sbc'
            )
            """
        ).fetchall()
    }
    assert catalogued == {
        "footnote_pension",
        "footnote_deferred_tax",
        "footnote_lease",
        "footnote_sbc",
    }
