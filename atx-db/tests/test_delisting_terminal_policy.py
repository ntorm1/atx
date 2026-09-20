"""Tier1-S4 T4: the explicit Shumway terminal-return policy and the coverage gate."""

from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


def _events():
    return pd.DataFrame(
        [
            {
                "security_id": "SEC-1",
                "symbol": "GONE",
                "delist_date": dt.date(2024, 1, 12),
                "as_of_date": dt.date(2024, 1, 12),
                "available_at": pd.Timestamp("2024-01-12 22:00:00"),
                "delist_reason": "exchange_delist",
                "successor_security_id": None,
            },
            {
                "security_id": "SEC-2",
                "symbol": "MERGED",
                "delist_date": dt.date(2024, 2, 1),
                "as_of_date": dt.date(2024, 2, 1),
                "available_at": pd.Timestamp("2024-02-01 22:00:00"),
                "delist_reason": "merger_acquisition",
                "successor_security_id": "SEC-3",
            },
        ]
    )


def _policy_dim(store):
    from atx_db.delisting import load_terminal_return_policy_dim

    return load_terminal_return_policy_dim(store)


def test_the_policy_row_exists_and_carries_the_shumway_default(tmp_store):
    from atx_db.delisting import (
        PERFORMANCE_TERMINAL_RETURN_POLICY_CODE,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
    )

    row = tmp_store.con.execute(
        "SELECT corporate_action_type, terminal_return_basis, default_return, is_observed_required "
        "FROM terminal_return_policy_dim WHERE policy_code = ?",
        [PERFORMANCE_TERMINAL_RETURN_POLICY_CODE],
    ).fetchone()
    assert row is not None
    assert row[0] == "performance_delist"
    assert row[1] == "shumway_default"
    assert float(row[2]) == SHUMWAY_PERFORMANCE_DELISTING_RETURN
    assert bool(row[3]) is False


@pytest.mark.parametrize(
    "code", ["SEC_FORM_25", "SEC_FORM_15", "ARCHIVE_LAST_TRADE", "NASDAQ_FINANCIAL_STATUS_BANKRUPT"]
)
def test_every_evidence_delist_code_is_seeded(tmp_store, code):
    count = tmp_store.con.execute("SELECT count(*) FROM delist_code_dim WHERE delist_code = ?", [code]).fetchone()[0]
    assert int(count) == 1


def test_the_policy_is_off_when_performance_return_is_none(tmp_store):
    from atx_db.delisting import apply_performance_delisting_policy

    out = apply_performance_delisting_policy(_events(), _policy_dim(tmp_store), performance_return=None)
    assert out.empty


def test_the_policy_only_fires_on_performance_reasons(tmp_store):
    from atx_db.delisting import POLICY_TERMINAL_RETURN_COLUMNS, apply_performance_delisting_policy

    out = apply_performance_delisting_policy(_events(), _policy_dim(tmp_store), performance_return=-0.30)
    assert list(out.columns) == list(POLICY_TERMINAL_RETURN_COLUMNS)
    assert list(out["security_id"]) == ["SEC-1"]
    assert float(out.iloc[0]["terminal_return"]) == pytest.approx(-0.30)
    assert out.iloc[0]["terminal_return_source"] == "policy"
    assert out.iloc[0]["terminal_return_policy"] == "performance_unknown"
    assert out.iloc[0]["return_basis"] == "shumway_default"


def test_the_policy_never_invents_an_available_at(tmp_store):
    from atx_db.delisting import apply_performance_delisting_policy

    events = _events()
    events.loc[0, "available_at"] = pd.NaT
    out = apply_performance_delisting_policy(events, _policy_dim(tmp_store), performance_return=-0.30)
    assert out.empty


def test_the_policy_is_row_order_independent(tmp_store):
    from atx_db.delisting import apply_performance_delisting_policy

    events = _events()
    policy = _policy_dim(tmp_store)
    a = apply_performance_delisting_policy(events, policy, performance_return=-0.30)
    b = apply_performance_delisting_policy(events.iloc[::-1].reset_index(drop=True), policy, performance_return=-0.30)
    pd.testing.assert_frame_equal(a, b)


def test_the_option_default_applies_the_shumway_convention(tmp_store):
    # S4 preflight ruling: the convention is ON by default; an operator opts out via
    # performance_delisting_return=None.
    from atx_db.delisting import (
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
        DelistingTerminalReturnOptions,
    )

    options = DelistingTerminalReturnOptions()
    assert options.performance_delisting_return == SHUMWAY_PERFORMANCE_DELISTING_RETURN

    opted_out = DelistingTerminalReturnOptions(performance_delisting_return=None)
    assert opted_out.performance_delisting_return is None


def test_compute_delisting_terminal_returns_applies_the_policy_independent_of_corporate_actions(
    tmp_store,
):
    # The performance policy must not hide behind corporate_actions availability: a
    # public-evidence-only warehouse has no licensed corporate-action feed at all.
    from atx_db.delisting import compute_delisting_terminal_returns

    events = _events()
    policy = _policy_dim(tmp_store)
    out = compute_delisting_terminal_returns(
        pd.DataFrame(),
        events,
        policy,
        corporate_actions=None,
        performance_delisting_return=-0.30,
    )
    assert list(out["security_id"]) == ["SEC-1"]
    assert out.iloc[0]["terminal_return_source"] == "policy"
    assert out.iloc[0]["terminal_return_policy"] == "performance_unknown"


def test_compute_delisting_terminal_returns_lets_observed_win_over_performance_policy(tmp_store):
    from atx_db.delisting import TERMINAL_RETURN_COLUMNS, compute_delisting_terminal_returns

    events = _events()
    policy = _policy_dim(tmp_store)
    observations = pd.DataFrame(
        [
            {
                "delisting_return_observation_id": "obs-1",
                "source": "vendor",
                "security_id": "SEC-1",
                "symbol": "GONE",
                "delist_date": dt.date(2024, 1, 12),
                "as_of_date": dt.date(2024, 1, 12),
                "available_at": pd.Timestamp("2024-01-12 22:00:00"),
                "source_loaded_at": pd.Timestamp("2024-01-12 22:00:00"),
                "crsp_dlstcd": 560,
                "delisting_return": -0.05,
                "delisting_return_ex_div": pd.NA,
                "return_basis": "CRSP_DLRET",
                "successor_security_id": pd.NA,
            }
        ]
    )
    out = compute_delisting_terminal_returns(
        observations,
        events,
        policy,
        corporate_actions=None,
        performance_delisting_return=-0.30,
    )
    assert set(out.columns) == set(TERMINAL_RETURN_COLUMNS)
    row = out[out["security_id"] == "SEC-1"].iloc[0]
    assert row["terminal_return_source"] == "observed"
    assert float(row["terminal_return"]) == pytest.approx(-0.05)


def _seed_one_delisting_event(store):
    store.con.execute(
        "INSERT INTO delisting_events (delisting_event_id, source, listing_status_source, "
        "source_listing_status_id, security_id, symbol, delist_date, as_of_date, available_at, "
        "delist_code, delist_reason, delisting_return_type, return_policy, return_confidence, "
        "evidence_source, evidence_source_table, method, evidence_confidence) VALUES "
        "('ev-1','atx_delisting_public_evidence_v1','nasdaq_delete','e-1','SEC-1','GONE',"
        "DATE '2024-01-12',DATE '2024-01-12',TIMESTAMP '2024-01-12 22:00:00','NASDAQ_DELETE',"
        "'exchange_delist','UNOBSERVED','none','none','public_evidence','nasdaq_listing_events',"
        "'public_evidence_precedence','high')"
    )


def _coverage_check(store):
    from atx_db.quality.checks_survivorship import (
        SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME,
        survivorship_check_specs,
    )

    spec = next(
        s for s in survivorship_check_specs() if s.check_name == SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME
    )
    return spec, float(store.con.execute(spec.sql).fetchone()[0])


def test_the_coverage_check_is_part_of_the_survivorship_specs():
    from atx_db.quality.checks_survivorship import (
        SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME,
        survivorship_check_specs,
    )

    names = {spec.check_name for spec in survivorship_check_specs()}
    assert SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME in names
    assert len(survivorship_check_specs()) == 3


def test_a_delisting_event_with_no_terminal_return_fails_the_coverage_check(tmp_store):
    _seed_one_delisting_event(tmp_store)
    spec, observed = _coverage_check(tmp_store)
    assert observed == 1.0
    assert spec.threshold == 0.0
    assert spec.comparator == "le"
    assert spec.severity == "error"


def test_the_coverage_check_clears_once_a_terminal_return_exists(tmp_store):
    _seed_one_delisting_event(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO delisting_terminal_returns (terminal_return_id, source, security_id, symbol, "
        "delist_date, as_of_date, available_at, terminal_return, terminal_return_source, "
        "terminal_return_policy, return_basis) VALUES "
        "('tr-1','atx_delisting_terminal_return_v1','SEC-1','GONE',DATE '2024-01-12',"
        "DATE '2024-01-12',TIMESTAMP '2024-01-12 22:00:00',-0.30,'policy','performance_unknown',"
        "'shumway_default')"
    )
    _spec, observed = _coverage_check(tmp_store)
    assert observed == 0.0


def test_the_coverage_check_is_registered(tmp_store):
    from atx_db.quality.checks_survivorship import SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME

    row = tmp_store.con.execute(
        "SELECT severity, threshold_value, comparator, enabled FROM quality_check_registry WHERE check_name = ?",
        [SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME],
    ).fetchone()
    assert row is not None
    assert row[0] == "error"
    assert float(row[1]) == 0.0
    assert row[2] == "le"
    assert bool(row[3]) is True
