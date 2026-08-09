"""Tests for the derived ``thirteenf_position_metrics`` dataset (S16).

The engine splits into a pure, DB-free transform (``compute_position_metrics``)
that maps per-(manager, security, report_period) common-share 13F holdings to
typed manager-level flow rows (quarter-over-quarter share change, NEW / ADDED /
TRIMMED / UNCHANGED / EXITED action, voting-authority concentration), and a thin
DuckDB materializer (``refresh_position_metrics`` / ``ThirteenFPositionMetricsDataset``)
that aggregates the duplicate holding lines per key and writes the result.

No network: metrics derive purely from already-cached warehouse tables
(``thirteenf_security_positions`` + ``thirteenf_manager_reports``).
"""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.thirteenf_position_metrics import (
    DEFAULT_SOURCE,
    ThirteenFPositionMetricsDataset,
    ThirteenFPositionMetricsOptions,
    compute_position_metrics,
    refresh_position_metrics,
    thirteenf_position_metrics_asof,
)


def _ts(s: str) -> pd.Timestamp:
    return pd.Timestamp(s)


def _pos(manager_id, security_id, report_period, shares, value, av, **over):
    base = {
        "manager_id": manager_id,
        "security_id": security_id,
        "symbol": "AAPL",
        "cusip": "037833100",
        "name_of_issuer": "APPLE INC",
        "report_period": report_period,
        "filing_date": report_period + dt.timedelta(days=45),
        "shares_held": shares,
        "value_usd": value,
        "portfolio_weight": 0.05,
        "voting_sole": shares,
        "voting_shared": 0.0,
        "voting_none": 0.0,
        "available_at": av,
    }
    base.update(over)
    return base


def _filed(manager_id, report_period, av):
    return {
        "manager_id": manager_id,
        "report_period": report_period,
        "filing_date": report_period + dt.timedelta(days=45),
        "available_at": av,
    }


Q1 = dt.date(2025, 3, 31)
Q2 = dt.date(2025, 6, 30)
Q3 = dt.date(2025, 9, 30)
AV1 = _ts("2025-05-15")
AV2 = _ts("2025-08-15")
AV3 = _ts("2025-11-15")


class TestComputePositionMetrics:
    def test_added_position(self):
        rows = pd.DataFrame([
            _pos("M1", "S1", Q1, 100.0, 1000.0, AV1),
            _pos("M1", "S1", Q2, 150.0, 1800.0, AV2),
        ])
        out = compute_position_metrics(rows, pd.DataFrame())
        by = {r.report_period: r for r in out.itertuples(index=False)}
        assert by[Q2].position_action == "ADDED"
        assert by[Q2].shares_held_prev == pytest.approx(100.0)
        assert by[Q2].shares_change == pytest.approx(50.0)
        assert by[Q2].shares_change_pct == pytest.approx(0.5)
        assert by[Q2].value_change == pytest.approx(800.0)
        assert bool(by[Q2].is_new_position) is False
        assert bool(by[Q2].is_closed_position) is False
        assert by[Q2].as_of_date == Q2

    def test_trimmed_position(self):
        rows = pd.DataFrame([
            _pos("M1", "S1", Q1, 150.0, 1800.0, AV1),
            _pos("M1", "S1", Q2, 100.0, 1100.0, AV2),
        ])
        out = compute_position_metrics(rows, pd.DataFrame())
        q2 = {r.report_period: r for r in out.itertuples(index=False)}[Q2]
        assert q2.position_action == "TRIMMED"
        assert q2.shares_change == pytest.approx(-50.0)
        assert q2.shares_change_pct == pytest.approx(-1 / 3)

    def test_unchanged_position(self):
        rows = pd.DataFrame([
            _pos("M1", "S1", Q1, 100.0, 1000.0, AV1),
            _pos("M1", "S1", Q2, 100.0, 1200.0, AV2),
        ])
        out = compute_position_metrics(rows, pd.DataFrame())
        q2 = {r.report_period: r for r in out.itertuples(index=False)}[Q2]
        assert q2.position_action == "UNCHANGED"
        assert q2.shares_change == pytest.approx(0.0)

    def test_new_position_has_no_prior(self):
        rows = pd.DataFrame([_pos("M1", "S1", Q2, 150.0, 1800.0, AV2)])
        out = compute_position_metrics(rows, pd.DataFrame())
        r = out.iloc[0]
        assert r["position_action"] == "NEW"
        assert pd.isna(r["shares_held_prev"])
        assert pd.isna(r["shares_change"])
        assert pd.isna(r["shares_change_pct"])
        assert bool(r["is_new_position"]) is True

    def test_nonconsecutive_quarter_is_new_not_continuation(self):
        # held Q1 and Q3 (Q2 skipped), and the manager only filed Q1 and Q3.
        # Q3 has no immediately-prior-quarter (Q2) holding -> NEW, and no exit
        # row is emitted for Q2 because the manager did not file Q2.
        rows = pd.DataFrame([
            _pos("M1", "S1", Q1, 100.0, 1000.0, AV1),
            _pos("M1", "S1", Q3, 200.0, 2400.0, AV3),
        ])
        filed = pd.DataFrame([_filed("M1", Q1, AV1), _filed("M1", Q3, AV3)])
        out = compute_position_metrics(rows, filed)
        actions = {r.report_period: r.position_action for r in out.itertuples(index=False)}
        assert actions[Q3] == "NEW"
        assert Q2 not in actions  # no exit row at Q2 (manager did not file Q2)

    def test_shares_change_pct_nan_when_prev_nonpositive(self):
        rows = pd.DataFrame([
            _pos("M1", "S1", Q1, 0.0, 0.0, AV1),
            _pos("M1", "S1", Q2, 100.0, 1000.0, AV2),
        ])
        out = compute_position_metrics(rows, pd.DataFrame())
        q2 = {r.report_period: r for r in out.itertuples(index=False)}[Q2]
        assert pd.isna(q2.shares_change_pct)
        assert q2.shares_change == pytest.approx(100.0)  # absolute change still defined

    def test_voting_sole_pct(self):
        rows = pd.DataFrame([
            _pos("M1", "S1", Q1, 100.0, 1000.0, AV1, voting_sole=80.0, voting_shared=20.0, voting_none=0.0),
        ])
        out = compute_position_metrics(rows, pd.DataFrame())
        assert out.iloc[0]["voting_sole_pct"] == pytest.approx(0.8)

    def test_exited_position_emits_synthetic_zero_row(self):
        # held Q1=100, filed Q2 but reported no holding of S1 -> EXITED at Q2.
        rows = pd.DataFrame([_pos("M1", "S1", Q1, 100.0, 1000.0, AV1)])
        filed = pd.DataFrame([_filed("M1", Q1, AV1), _filed("M1", Q2, AV2)])
        out = compute_position_metrics(rows, filed)
        by = {r.report_period: r for r in out.itertuples(index=False)}
        assert set(by) == {Q1, Q2}
        ex = by[Q2]
        assert ex.position_action == "EXITED"
        assert ex.shares_held == pytest.approx(0.0)
        assert ex.shares_held_prev == pytest.approx(100.0)
        assert ex.shares_change == pytest.approx(-100.0)
        assert ex.shares_change_pct == pytest.approx(-1.0)
        assert bool(ex.is_closed_position) is True
        assert ex.security_id == "S1"  # issuer identity carried from the prior holding
        assert pd.Timestamp(ex.available_at) == AV2  # availability is the Q2 filing instant

    def test_no_exit_row_when_not_filed_next_quarter(self):
        rows = pd.DataFrame([_pos("M1", "S1", Q1, 100.0, 1000.0, AV1)])
        filed = pd.DataFrame([_filed("M1", Q1, AV1)])  # did not file Q2
        out = compute_position_metrics(rows, filed)
        assert list(out["report_period"]) == [Q1]

    def test_no_exit_row_when_still_held(self):
        rows = pd.DataFrame([
            _pos("M1", "S1", Q1, 100.0, 1000.0, AV1),
            _pos("M1", "S1", Q2, 120.0, 1400.0, AV2),
        ])
        filed = pd.DataFrame([_filed("M1", Q1, AV1), _filed("M1", Q2, AV2)])
        out = compute_position_metrics(rows, filed)
        actions = {r.report_period: r.position_action for r in out.itertuples(index=False)}
        assert actions == {Q1: "NEW", Q2: "ADDED"}

    def test_metric_id_deterministic_and_unique(self):
        rows = pd.DataFrame([
            _pos("M1", "S1", Q1, 100.0, 1000.0, AV1),
            _pos("M2", "S1", Q1, 200.0, 2000.0, AV1),
            _pos("M1", "S1", Q2, 150.0, 1800.0, AV2),
        ])
        a = compute_position_metrics(rows, pd.DataFrame())
        b = compute_position_metrics(rows, pd.DataFrame())
        assert list(a["metric_id"]) == list(b["metric_id"])
        assert a["metric_id"].is_unique

    def test_empty_returns_typed_empty(self):
        out = compute_position_metrics(pd.DataFrame(), pd.DataFrame())
        assert out.empty
        assert "position_action" in out.columns


# --------------------------------------------------------------------------- #
# Integration: real thirteenf_security_positions rows -> position metrics
# --------------------------------------------------------------------------- #


def _insert_position(store, *, manager_id, security_id, report_period, shares, value,
                     av, line=0, is_common=True, put_call=None, voting_sole=None):
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
        ) VALUES (?, ?, ?, ?, ?, ?, 'AAPL', '037833100', 'APPLE INC', ?, ?, ?, ?, ?, ?, ?,
                  'SH', ?, ?, 0, 0, ?, 0.05, ?, ?, 'test', ?)
        """,
        [
            f"{accession}-{line}", f"{manager_id}-{report_period}", manager_id, accession, line,
            security_id, report_period, report_period + dt.timedelta(days=45),
            str(report_period), report_period, av, value, shares,
            put_call, (shares if voting_sole is None else voting_sole),
            (shares if voting_sole is None else voting_sole), is_common,
            bool(put_call), av,
        ],
    )


def _insert_report(store, *, manager_id, report_period, av):
    store.con.execute(
        """
        INSERT INTO thirteenf_manager_reports (
            manager_report_id, manager_id, accession_number, cik, report_period,
            filing_date, source_period, is_amendment, is_confidential_omitted,
            available_at, source, source_loaded_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, false, false, ?, 'test', ?)
        """,
        [
            f"{manager_id}-{report_period}", manager_id, f"{manager_id}-{report_period}",
            "0000000", report_period, report_period + dt.timedelta(days=45),
            str(report_period), av, av,
        ],
    )


class TestRefreshIntegration:
    def test_aggregates_duplicate_lines_into_one_key(self, tmp_store):
        # two common-share lines for the same (manager, security, period) sum into one row
        _insert_position(tmp_store, manager_id="M1", security_id="S1", report_period=Q1,
                         shares=60.0, value=600.0, av=dt.datetime(2025, 5, 15, 22), line=0)
        _insert_position(tmp_store, manager_id="M1", security_id="S1", report_period=Q1,
                         shares=40.0, value=400.0, av=dt.datetime(2025, 5, 15, 22), line=1)
        n = refresh_position_metrics(tmp_store, ThirteenFPositionMetricsOptions())
        assert n == 1
        row = tmp_store.con.execute(
            "SELECT shares_held, value_usd, position_action FROM thirteenf_position_metrics"
        ).fetchone()
        assert row[0] == pytest.approx(100.0)
        assert row[1] == pytest.approx(1000.0)
        assert row[2] == "NEW"

    def test_excludes_option_lines(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", security_id="S1", report_period=Q1,
                         shares=100.0, value=1000.0, av=dt.datetime(2025, 5, 15, 22), line=0)
        _insert_position(tmp_store, manager_id="M1", security_id="S1", report_period=Q1,
                         shares=999.0, value=9990.0, av=dt.datetime(2025, 5, 15, 22), line=1,
                         is_common=False, put_call="CALL")
        refresh_position_metrics(tmp_store, ThirteenFPositionMetricsOptions())
        row = tmp_store.con.execute("SELECT shares_held FROM thirteenf_position_metrics").fetchone()
        assert row[0] == pytest.approx(100.0)  # option line not summed in

    def test_materializes_added_action_across_quarters(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", security_id="S1", report_period=Q1,
                         shares=100.0, value=1000.0, av=dt.datetime(2025, 5, 15, 22))
        _insert_position(tmp_store, manager_id="M1", security_id="S1", report_period=Q2,
                         shares=150.0, value=1800.0, av=dt.datetime(2025, 8, 15, 22))
        refresh_position_metrics(tmp_store, ThirteenFPositionMetricsOptions())
        df = tmp_store.con.execute(
            "SELECT report_period, position_action, shares_change FROM thirteenf_position_metrics ORDER BY report_period"
        ).df()
        assert list(df["position_action"]) == ["NEW", "ADDED"]
        assert df.iloc[1]["shares_change"] == pytest.approx(50.0)

    def test_materializes_exit_from_manager_reports(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", security_id="S1", report_period=Q1,
                         shares=100.0, value=1000.0, av=dt.datetime(2025, 5, 15, 22))
        # manager filed both Q1 and Q2 but reported no S1 holding in Q2 -> EXITED at Q2
        _insert_report(tmp_store, manager_id="M1", report_period=Q1, av=dt.datetime(2025, 5, 15, 22))
        _insert_report(tmp_store, manager_id="M1", report_period=Q2, av=dt.datetime(2025, 8, 15, 22))
        refresh_position_metrics(tmp_store, ThirteenFPositionMetricsOptions())
        df = tmp_store.con.execute(
            "SELECT report_period, position_action, shares_held FROM thirteenf_position_metrics ORDER BY report_period"
        ).df()
        assert list(df["position_action"]) == ["NEW", "EXITED"]
        assert df.iloc[1]["shares_held"] == pytest.approx(0.0)

    def test_dataset_run_is_idempotent(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", security_id="S1", report_period=Q1,
                         shares=100.0, value=1000.0, av=dt.datetime(2025, 5, 15, 22))
        ds = ThirteenFPositionMetricsDataset()
        r1 = ds.run(tmp_store, ThirteenFPositionMetricsOptions())
        n1 = tmp_store.con.execute("SELECT count(*) FROM thirteenf_position_metrics").fetchone()[0]
        r2 = ds.run(tmp_store, ThirteenFPositionMetricsOptions())
        n2 = tmp_store.con.execute("SELECT count(*) FROM thirteenf_position_metrics").fetchone()[0]
        assert r1.rows_loaded == r2.rows_loaded
        assert n1 == n2 == 1


class TestAsofReader:
    def test_filters_by_available_at(self, tmp_store):
        _insert_position(tmp_store, manager_id="M1", security_id="S1", report_period=Q1,
                         shares=100.0, value=1000.0, av=dt.datetime(2025, 5, 15, 22))
        ThirteenFPositionMetricsDataset().run(tmp_store, ThirteenFPositionMetricsOptions())
        early = thirteenf_position_metrics_asof(dt.date(2025, 5, 1), store=tmp_store)
        assert early.empty
        late = thirteenf_position_metrics_asof(dt.date(2025, 12, 31), store=tmp_store)
        assert not late.empty
        assert set(late["manager_id"]) == {"M1"}
