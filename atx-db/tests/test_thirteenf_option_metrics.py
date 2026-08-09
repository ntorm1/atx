"""Tests for the derived ``thirteenf_option_metrics`` dataset (S20).

The engine maps cached SEC 13F holdings rows into issuer/report-period option
positioning aggregates: call/put share-equivalent exposure, put/call ratios,
common-share denominators, top option managers, option-bias labels, and QoQ
net-call changes. No network access: tests use pure pandas frames or inserted
warehouse rows.
"""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.thirteenf_option_metrics import (
    ThirteenFOptionMetricsDataset,
    ThirteenFOptionMetricsOptions,
    compute_option_metrics,
    refresh_option_metrics,
    thirteenf_option_metrics_asof,
)


def _ts(value: str) -> pd.Timestamp:
    return pd.Timestamp(value)


Q1 = dt.date(2025, 3, 31)
Q2 = dt.date(2025, 6, 30)
AV1 = _ts("2025-05-15 22:00:00")
AV2 = _ts("2025-08-15 22:00:00")


def _pos(manager_id, report_period, shares, value, av, *, put_call=None, is_common=False, **over):
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
        "is_option": bool(put_call),
    }
    base.update(over)
    return base


class TestComputeOptionMetrics:
    def test_aggregates_call_put_and_common_denominators(self):
        rows = pd.DataFrame([
            _pos("M1", Q1, 1000.0, 10000.0, AV1, is_common=True),
            _pos("M1", Q1, 100.0, 2000.0, AV1, put_call="CALL"),
            _pos("M2", Q1, 50.0, 1500.0, AV1, put_call="CALL"),
            _pos("M2", Q1, 25.0, 800.0, AV1, put_call="PUT"),
        ])
        out = compute_option_metrics(rows)
        assert len(out) == 1
        row = out.iloc[0]
        assert row["filing_count"] == 2
        assert row["option_manager_count"] == 2
        assert row["call_manager_count"] == 2
        assert row["put_manager_count"] == 1
        assert row["option_position_count"] == 3
        assert row["call_share_quantity"] == pytest.approx(150.0)
        assert row["put_share_quantity"] == pytest.approx(25.0)
        assert row["net_call_share_quantity"] == pytest.approx(125.0)
        assert row["put_call_share_ratio"] == pytest.approx(25.0 / 150.0)
        assert row["call_value_usd"] == pytest.approx(3500.0)
        assert row["put_value_usd"] == pytest.approx(800.0)
        assert row["option_value_usd"] == pytest.approx(4300.0)
        assert row["common_share_quantity"] == pytest.approx(1000.0)
        assert row["common_value_usd"] == pytest.approx(10000.0)
        assert row["call_to_common_share_pct"] == pytest.approx(0.15)
        assert row["put_to_common_share_pct"] == pytest.approx(0.025)
        assert row["option_to_common_value_pct"] == pytest.approx(0.43)
        assert row["option_bias"] == "CALL_HEAVY"
        assert row["as_of_date"] == Q1

    def test_top_managers_by_option_value(self):
        rows = pd.DataFrame([
            _pos("M1", Q1, 100.0, 2000.0, AV1, put_call="CALL"),
            _pos("M2", Q1, 100.0, 3000.0, AV1, put_call="CALL"),
            _pos("M3", Q1, 50.0, 900.0, AV1, put_call="PUT"),
            _pos("M4", Q1, 50.0, 1200.0, AV1, put_call="PUT"),
        ])
        row = compute_option_metrics(rows).iloc[0]
        assert row["top_call_manager_id"] == "M2"
        assert row["top_call_manager_value_usd"] == pytest.approx(3000.0)
        assert row["top_put_manager_id"] == "M4"
        assert row["top_put_manager_value_usd"] == pytest.approx(1200.0)

    def test_qoq_net_call_change_and_biases(self):
        rows = pd.DataFrame([
            _pos("M1", Q1, 100.0, 1000.0, AV1, put_call="CALL"),
            _pos("M2", Q1, 10.0, 100.0, AV1, put_call="PUT"),
            _pos("M1", Q2, 60.0, 700.0, AV2, put_call="CALL"),
            _pos("M2", Q2, 120.0, 1800.0, AV2, put_call="PUT"),
        ])
        out = compute_option_metrics(rows)
        by_period = {row.report_period: row for row in out.itertuples(index=False)}
        assert by_period[Q1].option_bias == "CALL_HEAVY"
        assert by_period[Q2].option_bias == "PUT_HEAVY"
        assert by_period[Q2].prior_report_period == Q1
        assert by_period[Q2].prior_net_call_share_quantity == pytest.approx(90.0)
        assert by_period[Q2].net_call_share_change == pytest.approx(-150.0)
        assert by_period[Q2].net_call_share_change_pct == pytest.approx(-150.0 / 90.0)

    def test_balanced_bias(self):
        rows = pd.DataFrame([
            _pos("M1", Q1, 50.0, 1000.0, AV1, put_call="CALL"),
            _pos("M2", Q1, 50.0, 1000.0, AV1, put_call="PUT"),
        ])
        row = compute_option_metrics(rows).iloc[0]
        assert row["option_bias"] == "BALANCED"

    def test_common_only_and_empty_return_typed_empty(self):
        common_only = compute_option_metrics(pd.DataFrame([_pos("M1", Q1, 100.0, 1000.0, AV1, is_common=True)]))
        assert common_only.empty
        assert "option_bias" in common_only.columns
        empty = compute_option_metrics(pd.DataFrame())
        assert empty.empty
        assert "put_call_value_ratio" in empty.columns


def _insert_position(
    store,
    *,
    manager_id,
    report_period,
    shares,
    value,
    av,
    line=0,
    is_common=False,
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
    def test_materializes_issuer_option_metrics(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", report_period=Q1, shares=1000.0,
                         value=10000.0, av=dt.datetime(2025, 5, 15, 22), is_common=True)
        _insert_position(tmp_store, manager_id="M1", report_period=Q1, shares=100.0,
                         value=2000.0, av=dt.datetime(2025, 5, 15, 22), put_call="CALL", line=1)
        _insert_position(tmp_store, manager_id="M2", report_period=Q1, shares=25.0,
                         value=800.0, av=dt.datetime(2025, 5, 15, 22), put_call="PUT")

        n = refresh_option_metrics(tmp_store, ThirteenFOptionMetricsOptions())
        assert n == 1
        row = tmp_store.con.execute(
            """
            SELECT call_share_quantity, put_share_quantity, common_share_quantity,
                   option_bias, top_call_manager_id, top_put_manager_id
            FROM thirteenf_option_metrics
            """
        ).fetchone()
        assert row[0] == pytest.approx(100.0)
        assert row[1] == pytest.approx(25.0)
        assert row[2] == pytest.approx(1000.0)
        assert row[3] == "CALL_HEAVY"
        assert row[4] == "M1"
        assert row[5] == "M2"

    def test_dataset_run_is_idempotent(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", report_period=Q1, shares=100.0,
                         value=2000.0, av=dt.datetime(2025, 5, 15, 22), put_call="CALL")
        dataset = ThirteenFOptionMetricsDataset()
        first = dataset.run(tmp_store, ThirteenFOptionMetricsOptions())
        n1 = tmp_store.con.execute("SELECT count(*) FROM thirteenf_option_metrics").fetchone()[0]
        second = dataset.run(tmp_store, ThirteenFOptionMetricsOptions())
        n2 = tmp_store.con.execute("SELECT count(*) FROM thirteenf_option_metrics").fetchone()[0]
        assert first.rows_loaded == second.rows_loaded
        assert n1 == n2 == 1


class TestAsofReader:
    def test_filters_by_available_at(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", report_period=Q1, shares=100.0,
                         value=2000.0, av=dt.datetime(2025, 5, 15, 22), put_call="CALL")
        ThirteenFOptionMetricsDataset().run(tmp_store, ThirteenFOptionMetricsOptions())
        early = thirteenf_option_metrics_asof(dt.date(2025, 5, 1), store=tmp_store)
        assert early.empty
        late = thirteenf_option_metrics_asof(dt.date(2025, 12, 31), store=tmp_store, symbols=["AAPL"])
        assert not late.empty
        assert set(late["symbol"]) == {"AAPL"}
