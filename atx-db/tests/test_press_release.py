from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.estimates import (
    EstimateMeasureSeedDataset,
    EstimateMeasureSeedOptions,
    EstimateSurpriseDataset,
    EstimateSurpriseOptions,
)
from atx_db.press_release import (
    SEC_EARNINGS_RELEASE_SOURCE,
    PressReleaseDataset,
    PressReleaseOptions,
    normalize_press_release_rows,
    press_release_facts_asof,
    refresh_press_release_reconciliation,
)
from atx_db.quality import run_warehouse_quality_checks

SECURITY_ID = "sec_pr_001"
SYMBOL = "PRCO"
CIK = "0000009999"
ACCESSION = "0000009999-25-000042"
PERIOD_START = dt.date(2025, 7, 1)
PERIOD_END = dt.date(2025, 9, 30)
RELEASE_AT = dt.datetime(2025, 10, 20, 8, 0, 0)
FINAL_AT = dt.datetime(2025, 11, 8, 16, 30, 0)


def _press_release_rows() -> pd.DataFrame:
    text = (
        "Item 2.02 Results of Operations and Financial Condition. "
        "For Q3 2025, revenue was $123.4 million. "
        "Non-GAAP diluted EPS was $2.10. "
        "GAAP net income was $50 million."
    )
    return pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "symbol": SYMBOL,
                "cik": CIK,
                "accession_number": ACCESSION,
                "form": "8-K",
                "items": "2.02;9.01",
                "filing_date": "2025-10-20",
                "release_datetime": RELEASE_AT.isoformat(sep=" "),
                "fiscal_year": 2025,
                "fiscal_period": "Q3",
                "period_end": PERIOD_END.isoformat(),
                "document_text": text,
                "source_url": "https://www.sec.gov/Archives/edgar/data/9999/prco-20251020.htm",
            }
        ]
    )


def _write_press_release_csv(tmp_path) -> object:
    source_file = tmp_path / "press_release.csv"
    _press_release_rows().to_csv(source_file, index=False)
    return source_file


def _companyfact(
    store, *, security_id: str, cik: str, concept: str, unit: str, start: dt.date, end: dt.date,
    value: float, accession: str, filed: dt.date, fiscal_year: int, fiscal_period: str, form: str,
) -> None:
    at = dt.datetime.combine(filed, dt.time()) + dt.timedelta(hours=46)
    store.con.execute(
        """
        INSERT INTO sec_company_facts (
            source, security_id, cik, taxonomy, concept, unit, period_start, period_end,
            filed_date, fiscal_year, fiscal_period, form, accession_number, value,
            available_at, source_url, source_loaded_at
        ) VALUES ('SEC companyfacts', ?, ?, 'us-gaap', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                  'https://example.invalid/companyfacts', ?)
        """,
        [security_id, cik, concept, unit, start, end, filed, fiscal_year, fiscal_period, form,
         accession, value, at, at],
    )


def _est_actual(
    store, *, security_id: str, measure_code: str, fiscal_year: int, fiscal_period: str,
    period_end: dt.date, value: float, unit: str, accession: str, available_at: dt.datetime,
    basis: str = "GAAP", form: str = "10-Q", period_start: dt.date | None = None,
) -> None:
    if period_start is None:
        # An est_actual row copies one Company Facts fact: take that fact's own start (0327 key).
        starts = store.con.execute(
            """
            SELECT DISTINCT period_start FROM sec_company_facts
            WHERE security_id = ? AND accession_number = ? AND period_end = ? AND value = ? AND unit = ?
            """,
            [security_id, accession, period_end, value, unit],
        ).fetchall()
        assert len(starts) == 1, f"no unique Company Facts geometry for {accession}: {starts}"
        period_start = starts[0][0]
    store.con.execute(
        """
        INSERT INTO est_actual (
            security_id, measure_code, fiscal_year, fiscal_period,
            period_start, period_end, duration_days, value, unit, basis, form, accession_number,
            announce_date, as_of_date, available_at, source
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'sec_company_facts')
        """,
        [security_id, measure_code, fiscal_year, fiscal_period, period_start, period_end,
         (period_end - period_start).days + 1, value, unit, basis,
         form, accession, available_at.date(), period_end, available_at],
    )


def _insert_final_actual(store, *, measure_code: str, value: float, basis: str = "GAAP") -> None:
    """A final actual backed by the exact Company Facts fact it was copied from."""

    EstimateMeasureSeedDataset().load(store, EstimateMeasureSeedOptions())
    concept = {"REVENUE": "Revenues", "EPS_DILUTED": "EarningsPerShareDiluted"}[measure_code]
    unit = "USD/shares" if measure_code.startswith("EPS") else "USD"
    accession = f"{ACCESSION}-FINAL-{measure_code}"
    _companyfact(
        store, security_id=SECURITY_ID, cik=CIK, concept=concept, unit=unit, start=PERIOD_START,
        end=PERIOD_END, value=value, accession=accession, filed=FINAL_AT.date(),
        fiscal_year=2025, fiscal_period="Q3", form="10-Q",
    )
    _est_actual(
        store, security_id=SECURITY_ID, measure_code=measure_code, fiscal_year=2025, fiscal_period="Q3",
        period_end=PERIOD_END, value=value, unit=unit, accession=accession, available_at=FINAL_AT,
        basis=basis,
    )


def _insert_fundamental_period(store) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_periods (
            fundamental_period_id,
            period_group_id,
            source,
            security_id,
            symbol,
            cik,
            period_start,
            period_end,
            datadate,
            period_days,
            normalized_period_type,
            calendar_year,
            calendar_quarter,
            calendar_period,
            rdq,
            pdate,
            fdate,
            ldate,
            as_of_date,
            available_at,
            form,
            accession_number,
            reported_fiscal_years_json,
            reported_fiscal_periods_json,
            statement_types_json,
            canonical_metrics_json,
            input_statement_point_ids_json,
            statement_point_count,
            canonical_metric_count,
            concept_count,
            value_changed_statement_count,
            has_balance_sheet,
            has_income_statement,
            has_cash_flow,
            has_per_share,
            revision_sequence,
            revision_count,
            is_latest_revision,
            first_available_at,
            latest_available_at,
            source_loaded_at
        )
        VALUES (
            'fp-pr-q3-2025',
            'fpg-pr-q3-2025',
            'fundamental_statement_points',
            ?,
            ?,
            ?,
            ?,
            ?,
            ?,
            92,
            'quarter',
            2025,
            3,
            '2025Q3',
            DATE '2025-11-08',
            DATE '2025-11-08',
            DATE '2025-11-08',
            DATE '2025-11-08',
            ?,
            ?,
            '10-Q',
            'final-10q-accession',
            '["2025"]',
            '["Q3"]',
            '["income_statement"]',
            '["revenue","eps_diluted"]',
            '["stmt-point-1"]',
            2,
            2,
            2,
            0,
            false,
            true,
            false,
            true,
            1,
            1,
            true,
            ?,
            ?,
            ?
        )
        """,
        [
            SECURITY_ID,
            SYMBOL,
            CIK,
            PERIOD_START,
            PERIOD_END,
            PERIOD_END,
            PERIOD_END,
            FINAL_AT,
            FINAL_AT,
            FINAL_AT,
            FINAL_AT,
        ],
    )


def test_press_release_text_extraction_tags_preliminary_facts() -> None:
    facts = normalize_press_release_rows(_press_release_rows(), options=PressReleaseOptions())

    by_measure = {row["measure_code"]: row for _, row in facts.iterrows()}
    assert by_measure["REVENUE"]["value"] == pytest.approx(123_400_000.0)
    assert by_measure["REVENUE"]["basis"] == "GAAP"
    assert by_measure["EPS_DILUTED"]["value"] == pytest.approx(2.10)
    assert by_measure["EPS_DILUTED"]["basis"] == "NON_GAAP"
    assert by_measure["NET_INCOME"]["value"] == pytest.approx(50_000_000.0)
    assert set(facts["is_preliminary"]) == {True}
    assert facts["source_file_sha256"].isna().all()
    assert all(facts["source_item"].str.contains("2.02"))


def test_press_release_refresh_reconciles_and_updates_period_dates(tmp_store, tmp_path) -> None:
    source_file = _write_press_release_csv(tmp_path)
    _insert_final_actual(tmp_store, measure_code="REVENUE", value=123_400_000.0)
    _insert_fundamental_period(tmp_store)

    result = PressReleaseDataset().run(
        tmp_store,
        PressReleaseOptions(source_file=source_file, min_confidence=0.70),
    )

    assert result.rows_loaded >= 3
    recon = tmp_store.con.execute(
        """
        SELECT reconciliation_status, final_actual_value
        FROM press_release_reconciliation
        WHERE measure_code = 'REVENUE'
        """
    ).fetchone()
    assert recon == ("matched_final", 123_400_000.0)

    dates = tmp_store.con.execute(
        "SELECT pdate, rdq FROM fundamental_periods WHERE security_id = ?",
        [SECURITY_ID],
    ).fetchone()
    assert dates == (RELEASE_AT.date(), RELEASE_AT.date())

    before_release = press_release_facts_asof(
        tmp_store,
        as_of_date=PERIOD_END,
        as_of_ts=RELEASE_AT - dt.timedelta(seconds=1),
    )
    after_release = press_release_facts_asof(
        tmp_store,
        as_of_date=PERIOD_END,
        as_of_ts=RELEASE_AT + dt.timedelta(seconds=1),
        security_ids=[SECURITY_ID],
    )
    assert before_release.empty
    assert len(after_release) >= 3

    checks = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=(
            "bad_press_release_fact_rows",
            "press_release_no_lookahead",
            "press_release_preliminary_vintage_retained",
        ),
    )
    assert {check.check_name: check.status for check in checks} == {
        "bad_press_release_fact_rows": "passed",
        "press_release_no_lookahead": "passed",
        "press_release_preliminary_vintage_retained": "passed",
    }


SEC_SECURITY = "SEC-CIK-0000009999"


def _sec_release(
    store, fact_id: str, *, measure_code: str, fiscal_year: int, fiscal_period: str,
    period_end: dt.date, value: float, available_at: dt.datetime,
) -> None:
    payload = json.dumps({"receipt_id": f"receipt-{fact_id}", "duration_evidence": "three_months_explicit"})
    store.con.execute(
        """
        INSERT INTO press_release_facts (
            press_release_fact_id, source, security_id, cik, accession_number, form,
            source_item, source_url, measure_code, fiscal_year, fiscal_period,
            period_end, value, unit, basis, is_preliminary, extraction_confidence,
            filing_date, release_date, as_of_date, available_at, raw_payload_json, run_id,
            input_codes_json
        ) VALUES (?, ?, ?, ?, ?, '8-K', '2.02 / EX-99', 'https://example.invalid/ex99.htm',
                  ?, ?, ?, ?, ?, 'USD_PER_SHARE', 'GAAP', true, 1.0, ?, ?, ?, ?, ?, 'fixture', '[]')
        """,
        [fact_id, SEC_EARNINGS_RELEASE_SOURCE, SEC_SECURITY, CIK, f"release-{fact_id}", measure_code,
         fiscal_year, fiscal_period, period_end, value, available_at.date(), available_at.date(),
         period_end, available_at, payload],
    )


def test_reconciliation_matches_period_geometry_not_fiscal_labels(tmp_store) -> None:
    """Final actuals match on owner, measure, period end and duration only.

    The Q2 2025 10-Q carries both the quarter and the six-month YTD value at
    the same end and filing labels (est_actual keeps the YTD one); the
    quarter's final value arrives as a comparative in the Q2 2026 10-Q under
    that filing's labels (2026, Q2).
    """

    store = tmp_store
    EstimateMeasureSeedDataset().load(store, EstimateMeasureSeedOptions())
    q2_end, q4_end = dt.date(2025, 6, 30), dt.date(2025, 12, 31)
    release_at = dt.datetime(2025, 7, 26, 22)
    _sec_release(store, "q2-basic", measure_code="EPS_BASIC", fiscal_year=2025, fiscal_period="Q2",
                 period_end=q2_end, value=1.40, available_at=release_at)
    _sec_release(store, "q2-diluted", measure_code="EPS_DILUTED", fiscal_year=2025, fiscal_period="Q2",
                 period_end=q2_end, value=1.38, available_at=release_at)
    _sec_release(store, "q4-basic", measure_code="EPS_BASIC", fiscal_year=2025, fiscal_period="Q4",
                 period_end=q4_end, value=0.50, available_at=dt.datetime(2026, 1, 30, 22))
    facts = (
        # concept, start, end, value, accession, filed, fiscal year/period, form
        ("EarningsPerShareBasic", dt.date(2025, 4, 1), q2_end, 1.40, "q2-2025", dt.date(2025, 8, 5), 2025, "Q2", "10-Q"),
        ("EarningsPerShareBasic", dt.date(2025, 1, 1), q2_end, 2.80, "q2-2025", dt.date(2025, 8, 5), 2025, "Q2", "10-Q"),
        ("EarningsPerShareDiluted", dt.date(2025, 4, 1), q2_end, 1.38, "q2-2025", dt.date(2025, 8, 5), 2025, "Q2", "10-Q"),
        ("EarningsPerShareBasic", dt.date(2025, 4, 1), q2_end, 1.40, "q2-2026", dt.date(2026, 8, 4), 2026, "Q2", "10-Q"),
        ("EarningsPerShareBasic", dt.date(2025, 1, 1), q4_end, 3.10, "fy-2025", dt.date(2026, 2, 20), 2025, "FY", "10-K"),
    )
    for concept, start, end, value, accession, filed, fiscal_year, fiscal_period, form in facts:
        _companyfact(
            store, security_id=SEC_SECURITY, cik=CIK, concept=concept, unit="USD/shares", start=start,
            end=end, value=value, accession=accession, filed=filed, fiscal_year=fiscal_year,
            fiscal_period=fiscal_period, form=form,
        )
    finals = (
        # est_actual keeps one row per (measure, filing labels, accession).
        ("EPS_BASIC", 2025, "Q2", q2_end, 2.80, "q2-2025", dt.datetime(2025, 8, 6, 22), "10-Q"),
        ("EPS_DILUTED", 2025, "Q2", q2_end, 1.38, "q2-2025", dt.datetime(2025, 8, 6, 22), "10-Q"),
        ("EPS_BASIC", 2026, "Q2", q2_end, 1.40, "q2-2026", dt.datetime(2026, 8, 5, 22), "10-Q"),
        ("EPS_BASIC", 2025, "FY", q4_end, 3.10, "fy-2025", dt.datetime(2026, 2, 21, 22), "10-K"),
    )
    for measure_code, fiscal_year, fiscal_period, end, value, accession, at, form in finals:
        _est_actual(
            store, security_id=SEC_SECURITY, measure_code=measure_code, fiscal_year=fiscal_year,
            fiscal_period=fiscal_period, period_end=end, value=value, unit="USD/shares",
            accession=accession, available_at=at, form=form,
        )

    assert refresh_press_release_reconciliation(
        store, PressReleaseOptions(source=SEC_EARNINGS_RELEASE_SOURCE)
    ) == 3
    rows = store.con.execute(
        """
        SELECT press_release_fact_id, fiscal_year, fiscal_period, final_actual_value,
               final_actual_accession_number, reconciliation_status
        FROM press_release_reconciliation WHERE source = ? ORDER BY press_release_fact_id
        """,
        [SEC_EARNINGS_RELEASE_SOURCE],
    ).fetchall()
    assert rows == [
        # The YTD value under matching labels is not this quarter's final; the
        # comparative under the next year's labels is.
        ("q2-basic", 2025, "Q2", 1.40, "q2-2026", "matched_final"),
        ("q2-diluted", 2025, "Q2", 1.38, "q2-2025", "matched_final"),
        # An annual value sharing the Q4 end is never a fourth-quarter final.
        ("q4-basic", 2025, "Q4", None, None, "pending_final"),
    ]


def _insert_sue_actual(store, fy: int, value: float) -> None:
    period_end = dt.date(fy, 12, 31)
    # A fourth-quarter (3-month) actual: the assertions are about the Q4 surprise vs a Q4 consensus.
    period_start = dt.date(fy, 10, 1)
    available_at = dt.datetime(fy + 1, 2, 10, 9, 0, 0)
    store.con.execute(
        """
        INSERT INTO est_actual (
            security_id, measure_code, fiscal_year, fiscal_period,
            period_start, period_end, duration_days, value, unit, basis, form, accession_number,
            announce_date, as_of_date, available_at, source
        )
        VALUES ('sec_basis_sue', 'EPS_DILUTED', ?, 'Q4', ?, ?, ?, ?, 'USD_PER_SHARE',
                'GAAP', '10-K', ?, ?, ?, ?, 'sec_company_facts')
        """,
        [fy, period_start, period_end, (period_end - period_start).days + 1, value, f"basis-sue-{fy}",
         available_at.date(), period_end, available_at],
    )


def _seed_basis_sue_actuals(store) -> None:
    for fy, value in [
        (2019, 1.00),
        (2020, 1.20),
        (2021, 1.50),
        (2022, 1.90),
        (2023, 2.35),
        (2024, 2.85),
    ]:
        _insert_sue_actual(store, fy, value)


def _insert_consensus(store, *, basis: str | None) -> None:
    columns = """
        security_id, measure_code, fiscal_year, fiscal_period, period_end,
        consensus_date, mean, basis, available_at, as_of_date, source
    """
    store.con.execute(
        f"""
        INSERT INTO est_consensus ({columns})
        VALUES ('sec_basis_sue', 'EPS_DILUTED', 2024, 'Q4', DATE '2024-12-31',
                DATE '2025-01-15', 2.70, ?, TIMESTAMP '2025-01-15 09:00:00',
                DATE '2024-12-31', 'test')
        """,
        [basis],
    )


def test_surprise_pct_is_suppressed_on_basis_mismatch(tmp_store) -> None:
    _seed_basis_sue_actuals(tmp_store)
    _insert_consensus(tmp_store, basis="STREET")

    EstimateSurpriseDataset().run(tmp_store, EstimateSurpriseOptions(min_obs=4))

    row = tmp_store.con.execute(
        """
        SELECT actual_basis, consensus_basis, basis_mismatch, consensus_mean, surprise_pct
        FROM est_surprise
        WHERE security_id = 'sec_basis_sue'
          AND measure_code = 'EPS_DILUTED'
          AND fiscal_year = 2024
        """
    ).fetchone()
    assert row == ("GAAP", "STREET", True, 2.70, None)

    tmp_store.con.execute("DELETE FROM est_consensus")
    _insert_consensus(tmp_store, basis="GAAP")
    EstimateSurpriseDataset().run(tmp_store, EstimateSurpriseOptions(min_obs=4))
    row = tmp_store.con.execute(
        """
        SELECT basis_mismatch, surprise_pct
        FROM est_surprise
        WHERE security_id = 'sec_basis_sue'
          AND measure_code = 'EPS_DILUTED'
          AND fiscal_year = 2024
        """
    ).fetchone()
    assert row[0] is False
    assert row[1] == pytest.approx((2.85 - 2.70) / 2.70, abs=1e-6)


def test_est_actual_eps_basis_quality_gate(tmp_store) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO est_actual (
            security_id, measure_code, fiscal_year, fiscal_period,
            period_start, period_end, duration_days, value, unit, form, accession_number,
            announce_date, as_of_date, available_at, source
        )
        VALUES ('sec_missing_basis', 'EPS_DILUTED', 2025, 'Q1',
                DATE '2025-01-01', DATE '2025-03-31', 90, 1.23, 'USD_PER_SHARE', '10-Q', 'missing-basis',
                DATE '2025-05-01', DATE '2025-03-31', TIMESTAMP '2025-05-01 08:00:00',
                'test')
        """
    )
    results = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=("est_actual_eps_missing_basis",),
    )
    assert results[0].status == "failed"
    assert results[0].severity == "critical"
