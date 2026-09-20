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
        DelistingTerminalReturnOptions,
        ShumwayPerformancePolicy,
    )

    options = DelistingTerminalReturnOptions()
    assert options.performance_delisting_return == ShumwayPerformancePolicy()

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
    from atx_db.delisting import (
        TERMINAL_RETURN_COLUMNS,
        ShumwayPerformancePolicy,
        compute_delisting_terminal_returns,
    )

    events = _events()
    events["listing_exchange_code"] = "XNAS"
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
        performance_delisting_return=ShumwayPerformancePolicy(),
    )
    assert set(out.columns) == set(TERMINAL_RETURN_COLUMNS)
    row = out[out["security_id"] == "SEC-1"].iloc[0]
    assert row["terminal_return_source"] == "observed"
    assert float(row["terminal_return"]) == pytest.approx(-0.05)


def test_performance_policy_dispatches_a_mixed_exchange_universe(tmp_store):
    from atx_db.delisting import ShumwayPerformancePolicy, apply_performance_delisting_policy

    exchanges = ["XNAS", "NASDAQ", " xnas ", "XNYS", "XASE", None]
    events = pd.concat([_events().iloc[[0]]] * len(exchanges), ignore_index=True)
    events["security_id"] = [f"SEC-{i}" for i in range(len(exchanges))]
    events["listing_exchange_code"] = exchanges
    policy = _policy_dim(tmp_store)
    out = apply_performance_delisting_policy(events, policy, performance_return=ShumwayPerformancePolicy())
    assert out["terminal_return"].tolist() == [-0.55] * 3 + [-0.30] * 3
    assert out["terminal_return_policy"].tolist() == ["performance_unknown_nasdaq"] * 3 + ["performance_unknown"] * 3
    assert out["return_basis"].tolist() == ["shumway_nasdaq_default"] * 3 + ["shumway_default"] * 3
    pd.testing.assert_frame_equal(
        out,
        apply_performance_delisting_policy(events.iloc[::-1], policy, performance_return=ShumwayPerformancePolicy()),
    )
    override = apply_performance_delisting_policy(events, policy, performance_return=-0.42)
    assert override["terminal_return"].tolist() == [-0.42] * len(exchanges)


def _seed_exchange_input(
    store,
    source,
    exchange,
    *,
    snapshot="2024-01-10",
    available="2024-01-10 22:00:00",
    valid_from="2024-01-10",
    valid_to=None,
    latest=True,
):
    if source == "membership":
        store.con.execute(
            "INSERT INTO universe_us_listed_membership (membership_id, universe_id, security_id, "
            "symbol, valid_from, valid_to, available_at, security_type, exchange_code, has_cik, "
            "reason, rules_json, decision_count, as_of_date, source, is_latest_revision) VALUES "
            "(?, 'us_listed_v1', 'SEC-1', 'GONE', ?, ?, ?, 'common', ?, false, 'member', '{}', 1, ?, 'test', ?)",
            [source + exchange + snapshot, valid_from, valid_to, available, exchange, snapshot, latest],
        )
    elif source == "listing":
        store.con.execute(
            "INSERT INTO exchange_listings (security_id, ticker, exchange_code, valid_from, "
            "valid_to, as_of_date, available_at, source, is_latest_revision) "
            "VALUES ('SEC-1', 'GONE', ?, ?, ?, ?, ?, 'test', ?)",
            [exchange, valid_from, valid_to, snapshot, available, latest],
        )
    else:
        store.con.execute(
            "INSERT INTO nasdaq_symbol_directory (directory, symbol, exchange, as_of_date, "
            "available_at, source_url, is_latest_revision) VALUES (?, 'GONE', ?, ?, ?, 'test', ?)",
            ["nasdaqlisted" if exchange == "XNAS" else "otherlisted", exchange, snapshot, available, latest],
        )


@pytest.mark.parametrize(
    ("inputs", "expected"),
    [
        ([("membership", "XNAS", {}), ("listing", "XNYS", {}), ("directory", "XNYS", {})], -0.55),
        ([("membership", "XNYS", {}), ("listing", "XNAS", {}), ("directory", "XNAS", {})], -0.30),
        ([("listing", "XNAS", {}), ("directory", "XNYS", {})], -0.55),
        ([("listing", "XNYS", {}), ("directory", "XNAS", {})], -0.30),
        ([("directory", "XNAS", {})], -0.55),
        ([("directory", "XNYS", {}), ("directory", "XNAS", {"snapshot": "2024-01-13"})], -0.30),
        (
            [
                ("directory", "XNYS", {}),
                ("directory", "XNAS", {"snapshot": "2024-01-11", "available": "2024-01-13 10:00:00"}),
            ],
            -0.30,
        ),
        ([("membership", "XNAS", {"snapshot": "2024-01-13"}), ("listing", "XNYS", {})], -0.30),
        ([("membership", "XNAS", {"valid_from": "2024-01-13"}), ("directory", "XNYS", {})], -0.30),
        ([("membership", "XNAS", {"valid_to": "2024-01-11"}), ("listing", "XNYS", {})], -0.30),
        ([("listing", "XNAS", {"valid_to": "2024-01-12"}), ("directory", "XNYS", {})], -0.30),
        ([("listing", "XNAS", {"available": "2024-01-12 23:00:00"})], -0.30),
        ([("membership", "XNAS", {"latest": False}), ("directory", "XNYS", {})], -0.30),
        ([], -0.30),
    ],
)
def test_refresh_resolves_exchange_in_pit_priority_order(tmp_store, inputs, expected):
    from atx_db.delisting import refresh_delisting_terminal_returns

    _seed_one_delisting_event(tmp_store)
    for source, exchange, overrides in inputs:
        _seed_exchange_input(tmp_store, source, exchange, **overrides)
    assert refresh_delisting_terminal_returns(tmp_store) == 1
    row = tmp_store.con.execute(
        "SELECT terminal_return, terminal_return_policy FROM delisting_terminal_returns"
    ).fetchone()
    assert row[0] == expected
    assert row[1] == ("performance_unknown_nasdaq" if expected == -0.55 else "performance_unknown")


def test_refresh_uses_maximum_event_and_selected_exchange_availability_and_can_opt_out(tmp_store):
    from atx_db.delisting import DelistingTerminalReturnOptions, refresh_delisting_terminal_returns

    _seed_one_delisting_event(tmp_store)
    tmp_store.con.execute("UPDATE delisting_events SET available_at = TIMESTAMP '2024-01-12 10:00:00'")
    _seed_exchange_input(tmp_store, "membership", "XNAS", available="2024-01-12 21:00:00")
    _seed_exchange_input(tmp_store, "listing", "XNYS", available="2024-01-12 22:00:00")
    assert refresh_delisting_terminal_returns(tmp_store) == 1
    assert tmp_store.con.execute("SELECT available_at FROM delisting_terminal_returns").fetchone()[0] == (
        dt.datetime(2024, 1, 12, 21)
    )
    assert (
        refresh_delisting_terminal_returns(tmp_store, DelistingTerminalReturnOptions(performance_delisting_return=None))
        == 0
    )


@pytest.mark.parametrize(
    ("code", "vendor_code", "expected"),
    [
        ("SEC_FORM_25", 500, "match"),
        ("SEC_FORM_25", 300, "match"),
        ("SEC_FORM_15", 500, "match"),
        ("ARCHIVE_LAST_TRADE", 500, "match"),
        ("NASDAQ_FINANCIAL_STATUS_BANKRUPT", 574, "match"),
        ("NASDAQ_FINANCIAL_STATUS_BANKRUPT", 400, "match"),
        ("SEC_FORM_15", 200, "mismatch"),
    ],
)
def test_public_evidence_codes_reconcile_with_vendor_families(tmp_store, code, vendor_code, expected):
    from atx_db.delisting import compute_delisting_code_reconciliation

    events = _events().iloc[[0]].assign(delisting_event_id="event-1", delist_code=code)
    observations = _events().iloc[[0]].assign(return_observation_id="obs-1", crsp_dlstcd=vendor_code)
    codes = tmp_store.con.execute("SELECT delist_code, reason_category FROM delist_code_dim").df()
    result = compute_delisting_code_reconciliation(events, observations, codes)
    assert result["reconciliation_status"].tolist() == [expected]


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
