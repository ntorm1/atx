from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from db.estimates import EstimateSurpriseDataset, EstimateSurpriseOptions
from db.press_release import (
    PressReleaseDataset,
    PressReleaseOptions,
    normalize_press_release_rows,
    press_release_facts_asof,
)
from db.quality import run_warehouse_quality_checks


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


def _insert_final_actual(store, *, measure_code: str, value: float, basis: str = "GAAP") -> None:
    store.con.execute(
        """
        INSERT INTO est_actual (
            security_id, measure_code, fiscal_year, fiscal_period,
            period_end, value, unit, basis, form, accession_number,
            announce_date, as_of_date, available_at, source
        )
        VALUES (?, ?, 2025, 'Q3', ?, ?, ?, ?, '10-Q', ?, ?, ?, ?, 'sec_company_facts')
        """,
        [
            SECURITY_ID,
            measure_code,
            PERIOD_END,
            value,
            "USD_PER_SHARE" if measure_code.startswith("EPS") else "USD",
            basis,
            f"{ACCESSION}-FINAL-{measure_code}",
            FINAL_AT.date(),
            PERIOD_END,
            FINAL_AT,
        ],
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


def _insert_sue_actual(store, fy: int, value: float) -> None:
    period_end = dt.date(fy, 12, 31)
    available_at = dt.datetime(fy + 1, 2, 10, 9, 0, 0)
    store.con.execute(
        """
        INSERT INTO est_actual (
            security_id, measure_code, fiscal_year, fiscal_period,
            period_end, value, unit, basis, form, accession_number,
            announce_date, as_of_date, available_at, source
        )
        VALUES ('sec_basis_sue', 'EPS_DILUTED', ?, 'Q4', ?, ?, 'USD_PER_SHARE',
                'GAAP', '10-K', ?, ?, ?, ?, 'sec_company_facts')
        """,
        [fy, period_end, value, f"basis-sue-{fy}", available_at.date(), period_end, available_at],
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
            period_end, value, unit, form, accession_number,
            announce_date, as_of_date, available_at, source
        )
        VALUES ('sec_missing_basis', 'EPS_DILUTED', 2025, 'Q1',
                DATE '2025-03-31', 1.23, 'USD_PER_SHARE', '10-Q', 'missing-basis',
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
