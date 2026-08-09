from __future__ import annotations

import datetime as dt

import pandas as pd

from atx_db.quality import run_warehouse_quality_checks
from atx_db.segments import (
    SegmentOptions,
    compute_segment_rows,
    infer_segment_type,
    refresh_segment_footnote_coverage,
    refresh_segments,
)


SECURITY_ID = "SEC-CIK-0000000007"
CIK = "0000000007"
SYMBOL = "SEG"
ACCESSION = "0000000007-25-000001"
PERIOD_START = dt.date(2024, 1, 1)
PERIOD_END = dt.date(2024, 12, 31)
AVAILABLE_AT = dt.datetime(2025, 2, 15, 18, 0)


def _candidate(member: str, value: float, fact_id: str) -> dict:
    return {
        "filing_fact_id": fact_id,
        "filing_context_id": f"ctx-{member}",
        "filing_dimension_id": f"dim-{member}",
        "security_id": SECURITY_ID,
        "symbol": SYMBOL,
        "cik": CIK,
        "accession_number": ACCESSION,
        "form": "10-K",
        "filing_date": AVAILABLE_AT.date(),
        "acceptance_datetime": AVAILABLE_AT,
        "primary_document": "seg-2024.htm",
        "context_id": f"c-{member}",
        "period_type": "duration",
        "period_start": PERIOD_START,
        "period_end": PERIOD_END,
        "instant_date": None,
        "dimension_count": 1,
        "context_element": "segment",
        "member_kind": "explicit",
        "dimension_qname": "us-gaap:StatementBusinessSegmentsAxis",
        "dimension_taxonomy": "us-gaap",
        "dimension_concept": "StatementBusinessSegmentsAxis",
        "member_qname": f"example:{member}SegmentMember",
        "member_taxonomy": "example",
        "member_concept": f"{member}SegmentMember",
        "typed_member_value": None,
        "member_text": member,
        "ordinal": 0,
        "concept": "RevenueFromContractWithCustomerExcludingAssessedTax",
        "taxonomy": "us-gaap",
        "unit": "USD",
        "value": value,
        "raw_value": str(value),
        "source_url": "https://www.sec.gov/Archives/edgar/data/7/seg-2024.htm",
        "available_at": AVAILABLE_AT,
        "source_loaded_at": AVAILABLE_AT + dt.timedelta(minutes=5),
    }


def _consolidated(value: float) -> pd.DataFrame:
    return pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "symbol": SYMBOL,
                "cik": CIK,
                "canonical_code": "revenue",
                "period_start": PERIOD_START,
                "period_end": PERIOD_END,
                "basis": "annual",
                "value": value,
                "available_at": AVAILABLE_AT,
                "is_latest_revision": True,
            }
        ]
    )


def test_compute_segment_rows_reconciles_and_flags_divergence() -> None:
    candidates = pd.DataFrame([_candidate("North", 60.0, "fact-north"), _candidate("South", 40.0, "fact-south")])

    _dim, facts = compute_segment_rows(candidates, _consolidated(100.0), source="fixture")
    assert set(facts["reconciliation_status"]) == {"reconciled"}
    assert set(facts["segment_sum_value"]) == {100.0}

    _dim, facts = compute_segment_rows(
        candidates,
        _consolidated(130.0),
        source="fixture",
        reconciliation_tolerance=0.02,
    )
    assert set(facts["reconciliation_status"]) == {"flagged_divergent"}
    assert facts["reconciliation_relative_difference"].iloc[0] > 0.02


def test_segment_axis_type_inference_keeps_unclassified_axes() -> None:
    assert infer_segment_type("us-gaap:StatementBusinessSegmentsAxis", None) == "business"
    assert infer_segment_type("srt:StatementGeographicalAxis", None) == "geographic"
    assert infer_segment_type("example:CustomSegmentAxis", "CustomSegmentAxis") == "unclassified"


def _seed_security(store) -> None:
    store.con.execute(
        "INSERT OR REPLACE INTO securities (security_id, primary_symbol, source) VALUES (?, ?, ?)",
        [SECURITY_ID, SYMBOL, "test"],
    )


def _insert_dimensional_context(store, *, member: str, context_id: str) -> None:
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
            DATE '2025-02-15', DATE '2024-12-31', TIMESTAMP '2025-02-15 18:00:00', 'seg-2024.htm',
            ?, 'http://www.sec.gov/CIK', ?,
            'duration', DATE '2024-01-01', DATE '2024-12-31', NULL,
            true, false, 1, 0,
            1, ?, 'https://www.sec.gov/Archives/edgar/data/7/seg-2024.htm', 'test', TIMESTAMP '2025-02-15 18:05:00'
        )
        """,
        [context_id, SECURITY_ID, CIK, ACCESSION, f"c-{member}", CIK, f"hash-{member}"],
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
            ?, ?, ?, ?, ?, '10-K', DATE '2025-02-15', TIMESTAMP '2025-02-15 18:00:00',
            'seg-2024.htm', ?, 'segment', 'explicit',
            'us-gaap:StatementBusinessSegmentsAxis', 'us-gaap', 'StatementBusinessSegmentsAxis',
            ?, 'example', ?, NULL, ?, 0,
            'https://www.sec.gov/Archives/edgar/data/7/seg-2024.htm', 'test', TIMESTAMP '2025-02-15 18:05:00'
        )
        """,
        [
            f"dim-{member}",
            context_id,
            SECURITY_ID,
            CIK,
            ACCESSION,
            f"c-{member}",
            f"example:{member}SegmentMember",
            f"{member}SegmentMember",
            member,
        ],
    )


def _insert_fact(store, *, fact_id: str, context_id: str, ordinal: int, value: float) -> None:
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
            ?, ?, ?, ?, ?, '10-K', DATE '2025-02-15', TIMESTAMP '2025-02-15 18:00:00',
            'seg-2024.htm', ?, 'nonFraction', NULL,
            'us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax', 'us-gaap',
            'RevenueFromContractWithCustomerExcludingAssessedTax', ?, 'USD',
            '["iso4217:USD"]', '[]', '[]', '0', NULL, NULL, NULL, NULL, NULL, false,
            CAST(? AS VARCHAR), CAST(? AS VARCHAR), ?, true, ?,
            'https://www.sec.gov/Archives/edgar/data/7/seg-2024.htm', 'test', TIMESTAMP '2025-02-15 18:05:00'
        )
        """,
        [fact_id, context_id, SECURITY_ID, CIK, ACCESSION, ordinal, f"c-{context_id}", value, value, value, ordinal],
    )


def _insert_standardized_revenue(store, value: float) -> None:
    store.con.execute("DELETE FROM fundamental_standardized WHERE security_id = ? AND canonical_code = 'revenue'", [SECURITY_ID])
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, upstream_source, security_id, symbol, cik,
            item_id, canonical_code, basis, period_start, period_end, value, unit_type,
            source_accession, filed_date, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, is_latest_revision
        )
        VALUES (
            ?, 'standardized_fixture', 'fixture', ?, ?, ?, 1001, 'revenue', 'annual',
            DATE '2024-01-01', DATE '2024-12-31', ?, 'monetary', ?, DATE '2025-02-15',
            DATE '2024-12-31', TIMESTAMP '2025-02-15 18:00:00', '["revenue"]', '[1001]',
            'fixture_rule', 'identity', true
        )
        """,
        [f"{SECURITY_ID}|revenue|{value}", SECURITY_ID, SYMBOL, CIK, value, ACCESSION],
    )


def _seed_segment_fixture(store) -> None:
    _seed_security(store)
    for ordinal, (member, value) in enumerate((("North", 60.0), ("South", 40.0)), start=1):
        context_id = f"ctx-{member}"
        _insert_dimensional_context(store, member=member, context_id=context_id)
        _insert_fact(store, fact_id=f"fact-{member}", context_id=context_id, ordinal=ordinal, value=value)


def test_refresh_segments_populates_tables_and_quality_warning(tmp_store) -> None:
    _seed_segment_fixture(tmp_store)
    _insert_standardized_revenue(tmp_store, 100.0)

    counts = refresh_segments(tmp_store, SegmentOptions(source="segment_fixture"))
    assert counts == {"segment_dim_rows": 2, "segment_fact_rows": 2}
    statuses = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT DISTINCT reconciliation_status FROM segment_fact WHERE source = 'segment_fixture'"
        ).fetchall()
    }
    assert statuses == {"reconciled"}

    _insert_standardized_revenue(tmp_store, 130.0)
    refresh_segments(tmp_store, SegmentOptions(source="segment_divergent"))
    result = run_warehouse_quality_checks(
        tmp_store,
        check_names=["segment_reconciliation_divergence_warning"],
        record=False,
    )[0]
    assert result.status == "warning"
    assert result.observed_value == 2.0
    assert result.details["rows"]

    coverage_rows = refresh_segment_footnote_coverage(tmp_store, SegmentOptions(run_id="test-run"))
    assert coverage_rows == 1
    coverage = tmp_store.con.execute(
        """
        SELECT segment_fact_count, segment_reconciled_count, segment_flagged_divergent_count
        FROM segment_footnote_coverage
        """
    ).fetchone()
    assert coverage == (4, 2, 2)


def test_segment_migrations_catalog_and_contract(tmp_store) -> None:
    versions = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
        ).fetchall()
    }
    assert {117, 119, 120}.issubset(versions)
    catalogued = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM table_catalog
            WHERE table_name IN ('segment_dim', 'segment_fact', 'segment_footnote_coverage')
            """
        ).fetchall()
    }
    assert catalogued == {"segment_dim", "segment_fact", "segment_footnote_coverage"}
