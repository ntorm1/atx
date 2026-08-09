"""Tests for the derived ``thirteenf_concentration_metrics`` dataset (S22)."""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.thirteenf_concentration_metrics import (
    ThirteenFConcentrationMetricsDataset,
    ThirteenFConcentrationMetricsOptions,
    compute_concentration_metrics,
    refresh_concentration_metrics,
    thirteenf_concentration_metrics_asof,
)


def _ts(value: str) -> pd.Timestamp:
    return pd.Timestamp(value)


Q1 = dt.date(2025, 3, 31)
Q2 = dt.date(2025, 6, 30)
AV1 = _ts("2025-05-15 22:00:00")
AV2 = _ts("2025-08-15 22:00:00")


def _pos(manager_id, report_period, shares, value, av, *, is_common=True, put_call=None, **over):
    base = {
        "manager_report_id": f"{manager_id}-{report_period}",
        "manager_id": manager_id,
        "accession_number": f"{manager_id}-{report_period}",
        "security_id": "S1",
        "symbol": "AAPL",
        "cusip": "037833100",
        "name_of_issuer": "APPLE INC",
        "report_period": report_period,
        "filing_date": report_period + dt.timedelta(days=45),
        "source_period": str(report_period),
        "available_at": av,
        "value_usd": value,
        "share_quantity": shares,
        "put_call": put_call,
        "portfolio_weight": 0.05,
        "is_common_share": is_common,
    }
    base.update(over)
    return base


class TestComputeConcentrationMetrics:
    def test_aggregates_top_holder_hhi_and_effective_count(self):
        rows = pd.DataFrame([
            _pos("M1", Q1, 600.0, 6000.0, AV1),
            _pos("M2", Q1, 300.0, 3000.0, AV1),
            _pos("M3", Q1, 100.0, 1000.0, AV1),
        ])
        out = compute_concentration_metrics(rows)
        assert len(out) == 1
        row = out.iloc[0]
        assert row["filing_count"] == 3
        assert row["holder_count"] == 3
        assert row["common_value_usd"] == pytest.approx(10000.0)
        assert row["top_holder_manager_id"] == "M1"
        assert row["top_holder_value_pct"] == pytest.approx(0.6)
        assert row["top_3_holder_value_pct"] == pytest.approx(1.0)
        assert row["top_5_holder_value_pct"] == pytest.approx(1.0)
        assert row["value_hhi"] == pytest.approx(0.46)
        assert row["share_hhi"] == pytest.approx(0.46)
        assert row["effective_holder_count_value"] == pytest.approx(1 / 0.46)
        assert row["concentration_bucket"] == "HIGHLY_CONCENTRATED"
        assert row["as_of_date"] == Q1

    def test_qoq_hhi_and_holder_count_changes(self):
        rows = pd.DataFrame([
            _pos("M1", Q1, 500.0, 5000.0, AV1),
            _pos("M2", Q1, 500.0, 5000.0, AV1),
            _pos("M1", Q2, 700.0, 7000.0, AV2),
            _pos("M2", Q2, 200.0, 2000.0, AV2),
            _pos("M3", Q2, 100.0, 1000.0, AV2),
        ])
        out = compute_concentration_metrics(rows)
        by_period = {row.report_period: row for row in out.itertuples(index=False)}
        assert by_period[Q1].value_hhi == pytest.approx(0.5)
        assert by_period[Q2].prior_report_period == Q1
        assert by_period[Q2].prior_value_hhi == pytest.approx(0.5)
        assert by_period[Q2].value_hhi == pytest.approx(0.54)
        assert by_period[Q2].value_hhi_change == pytest.approx(0.04)
        assert by_period[Q2].prior_holder_count == 2
        assert by_period[Q2].holder_count_change == 1

    def test_ignores_option_rows_and_empty_returns_typed_empty(self):
        option_only = compute_concentration_metrics(
            pd.DataFrame([_pos("M1", Q1, 100.0, 1000.0, AV1, is_common=False, put_call="CALL")])
        )
        assert option_only.empty
        assert "value_hhi" in option_only.columns
        empty = compute_concentration_metrics(pd.DataFrame())
        assert empty.empty
        assert "top_5_holder_value_pct" in empty.columns


def _insert_position(
    store,
    *,
    manager_id,
    report_period,
    shares,
    value,
    av,
    line=0,
    is_common=True,
    put_call=None,
):
    accession = f"{manager_id}-{report_period}-{line}"
    store.con.execute(
        """
        INSERT INTO thirteenf_security_positions (
            position_id, manager_report_id, manager_id, accession_number, infotable_sk,
            security_id, symbol, cusip, name_of_issuer, report_period, filing_date,
            source_period, as_of_date, available_at, value_usd, share_quantity,
            share_quantity_type, put_call, voting_auth_sole, voting_auth_shared,
            voting_auth_none, voting_auth_total, portfolio_weight, is_common_share,
            is_option, source, source_loaded_at
        ) VALUES (?, ?, ?, ?, ?, 'S1', 'AAPL', '037833100', 'APPLE INC', ?, ?, ?, ?, ?, ?, ?,
                  'SH', ?, ?, 0, 0, ?, 0.05, ?, ?, 'test', ?)
        """,
        [
            f"{accession}-{line}",
            f"{manager_id}-{report_period}",
            manager_id,
            accession,
            line,
            report_period,
            report_period + dt.timedelta(days=45),
            str(report_period),
            report_period,
            av,
            value,
            shares,
            put_call,
            shares,
            shares,
            is_common,
            bool(put_call),
            av,
        ],
    )


class TestRefreshIntegration:
    def test_materializes_issuer_concentration_metrics(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", report_period=Q1, shares=600.0,
                         value=6000.0, av=dt.datetime(2025, 5, 15, 22))
        _insert_position(tmp_store, manager_id="M2", report_period=Q1, shares=300.0,
                         value=3000.0, av=dt.datetime(2025, 5, 15, 22))
        _insert_position(tmp_store, manager_id="M3", report_period=Q1, shares=100.0,
                         value=1000.0, av=dt.datetime(2025, 5, 15, 22))

        n = refresh_concentration_metrics(tmp_store, ThirteenFConcentrationMetricsOptions())
        assert n == 1
        row = tmp_store.con.execute(
            """
            SELECT holder_count, top_holder_manager_id, top_holder_value_pct,
                   top_5_holder_value_pct, value_hhi, concentration_bucket
            FROM thirteenf_concentration_metrics
            """
        ).fetchone()
        assert row[0] == 3
        assert row[1] == "M1"
        assert row[2] == pytest.approx(0.6)
        assert row[3] == pytest.approx(1.0)
        assert row[4] == pytest.approx(0.46)
        assert row[5] == "HIGHLY_CONCENTRATED"

    def test_dataset_run_is_idempotent(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", report_period=Q1, shares=100.0,
                         value=2000.0, av=dt.datetime(2025, 5, 15, 22))
        dataset = ThirteenFConcentrationMetricsDataset()
        first = dataset.run(tmp_store, ThirteenFConcentrationMetricsOptions())
        n1 = tmp_store.con.execute("SELECT count(*) FROM thirteenf_concentration_metrics").fetchone()[0]
        second = dataset.run(tmp_store, ThirteenFConcentrationMetricsOptions())
        n2 = tmp_store.con.execute("SELECT count(*) FROM thirteenf_concentration_metrics").fetchone()[0]
        assert first.rows_loaded == second.rows_loaded
        assert n1 == n2 == 1


class TestAsofReader:
    def test_filters_by_available_at(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", report_period=Q1, shares=100.0,
                         value=2000.0, av=dt.datetime(2025, 5, 15, 22))
        ThirteenFConcentrationMetricsDataset().run(tmp_store, ThirteenFConcentrationMetricsOptions())
        early = thirteenf_concentration_metrics_asof(dt.date(2025, 5, 1), store=tmp_store)
        assert early.empty
        late = thirteenf_concentration_metrics_asof(dt.date(2025, 12, 31), store=tmp_store, symbols=["AAPL"])
        assert not late.empty
        assert set(late["symbol"]) == {"AAPL"}
