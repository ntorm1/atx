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


# ---------------------------------------------------------------------------
# A3: corporate-action consideration on the unadjusted close; loud empty-price failure; RX2.
# ---------------------------------------------------------------------------


def _seed_merger_event(store, *, security_id="SEC-1", delist_date="2024-01-15", reason="merger_acquisition"):
    store.con.execute(
        "INSERT INTO delisting_events (delisting_event_id, source, listing_status_source, "
        "source_listing_status_id, security_id, symbol, delist_date, as_of_date, available_at, "
        "delist_code, delist_reason, delisting_return_type, return_policy, return_confidence, "
        "evidence_source, evidence_source_table, method, evidence_confidence) VALUES "
        "(?, 'atx_delisting_public_evidence_v1', 'sec_form_25', 'e-1', ?, 'TGT', ?, ?, "
        "TIMESTAMP '2024-01-16 22:00:00', 'SEC_FORM_25', ?, 'UNOBSERVED', 'none', 'none', "
        "'public_evidence', 'sec_submissions', 'public_evidence_cessation_cluster', 'high')",
        [f"ev-{security_id}", security_id, delist_date, delist_date, reason],
    )


def _seed_cash_merger(store, *, security_id="SEC-1", ex_date="2024-01-15", cash=50.0):
    store.con.execute(
        "INSERT INTO corporate_actions (source, security_id, action_type, ex_date, cash_amount, available_at) "
        "VALUES ('test', ?, 'merger', ?, ?, TIMESTAMP '2024-01-16 09:00:00')",
        [security_id, ex_date, cash],
    )


def _seed_raw_and_adjusted_bar(store, day, close, adjusted, *, security_id="SEC-1"):
    store.con.execute(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, adjusted_close, "
        "available_at, as_of_date, is_latest_revision) VALUES ('test', ?, 'TGT', ?, ?, ?, ?, ?, true)",
        [security_id, day, close, adjusted, f"{day} 22:00:00", day],
    )


def test_cash_consideration_uses_the_unadjusted_last_pre_delist_close(tmp_store):
    from atx_db.delisting import refresh_delisting_terminal_returns

    _seed_merger_event(tmp_store)
    _seed_cash_merger(tmp_store)
    _seed_raw_and_adjusted_bar(tmp_store, "2024-01-11", 39.0, 37.0)
    _seed_raw_and_adjusted_bar(tmp_store, "2024-01-12", 40.0, 38.0)  # last trade, raw 40 / adjusted 38
    assert refresh_delisting_terminal_returns(tmp_store) == 1
    row = tmp_store.con.execute(
        "SELECT terminal_return, terminal_return_source, terminal_return_policy, return_basis, available_at "
        "FROM delisting_terminal_returns"
    ).fetchone()
    assert row[0] == pytest.approx(0.25)  # 50 / 40 - 1, not 50 / 38 - 1
    assert row[1:4] == ("policy", "merger_cash", "cash_consideration")
    assert row[4] == dt.datetime(2024, 1, 16, 9)  # max(action clock, last bar clock)
    assert tmp_store.con.execute(
        "SELECT status FROM data_quality_checks WHERE check_name = 'corporate_action_unadjusted_price_input'"
    ).fetchall() == [("passed",)]


def test_corporate_action_branch_fails_loudly_when_its_price_input_is_empty(tmp_store):
    from atx_db.delisting import refresh_delisting_terminal_returns

    _seed_merger_event(tmp_store)
    _seed_cash_merger(tmp_store)
    with pytest.raises(RuntimeError, match="price input"):
        refresh_delisting_terminal_returns(tmp_store)
    assert tmp_store.con.execute(
        "SELECT status, severity, observed_value FROM data_quality_checks "
        "WHERE check_name = 'corporate_action_unadjusted_price_input'"
    ).fetchall() == [("failed", "error", 1.0)]
    assert tmp_store.con.execute("SELECT count(*) FROM delisting_terminal_returns").fetchone()[0] == 0


def test_a_missing_pre_delist_close_is_counted_not_back_filled_from_adjusted(tmp_store):
    from atx_db.delisting import refresh_delisting_terminal_returns

    _seed_merger_event(tmp_store)
    _seed_merger_event(tmp_store, security_id="SEC-2")
    _seed_cash_merger(tmp_store)
    _seed_cash_merger(tmp_store, security_id="SEC-2")
    _seed_raw_and_adjusted_bar(tmp_store, "2024-01-12", 40.0, 38.0)
    _seed_raw_and_adjusted_bar(tmp_store, "2024-01-12", None, 38.0, security_id="SEC-2")  # adjusted only
    assert refresh_delisting_terminal_returns(tmp_store) == 1
    assert tmp_store.con.execute("SELECT security_id FROM delisting_terminal_returns").fetchall() == [("SEC-1",)]
    assert tmp_store.con.execute(
        "SELECT status, observed_value FROM data_quality_checks "
        "WHERE check_name = 'corporate_action_unadjusted_price_input'"
    ).fetchall() == [("warning", 1.0)]


@pytest.mark.parametrize(
    ("reason", "expected_rows"),
    [("merger_acquisition", 0), ("voluntary", 0), ("unknown", 1), ("exchange_delist", 1), ("bankruptcy", 1)],
)
def test_shumway_applies_only_to_unexplained_or_performance_cessations(tmp_store, reason, expected_rows):
    from atx_db.delisting import delisting_policy_bias_exposure, refresh_delisting_terminal_returns

    _seed_merger_event(tmp_store, reason=reason)
    assert refresh_delisting_terminal_returns(tmp_store) == expected_rows
    exposure = delisting_policy_bias_exposure(tmp_store)
    assert exposure["merger_or_voluntary_performance_policy_rows"] == 0
    # No details_json CIK on this hand-seeded event: an unknown one is CIK-less exposure.
    assert exposure["cik_less_unknown_performance_policy_rows"] == (1 if reason == "unknown" else 0)


# ---------------------------------------------------------------------------
# A3 fix round 1 (review I2): observed DLRET on the CRSP DLSTDT basis (last price date) is
# moved onto the event basis (first session after the last trade): one terminal per
# cessation, and the last traded session's return stays in the stitched label.
# ---------------------------------------------------------------------------


def _obs_sessions(count=140):
    day, out = dt.date(2023, 1, 2), []
    while len(out) < count:
        if day.weekday() < 5:
            out.append(day)
        day += dt.timedelta(days=1)
    return out


OBS_SESSIONS = _obs_sessions()
LAST = 99  # index of the observed security's last trade


def _seed_observed_cessation(store):
    from atx_db.delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    rows = [("SEC-LIVE", "LIVE", day, 10.0) for day in OBS_SESSIONS]
    # Flat at 10, then +20% on the last traded session.
    rows += [("SEC-OBS", "OBS", day, 12.0 if i == LAST else 10.0) for i, day in enumerate(OBS_SESSIONS[: LAST + 1])]
    store.con.executemany(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, adjusted_close, "
        "volume, available_at, as_of_date, is_latest_revision) VALUES ('test', ?, ?, ?, ?, ?, 1000, ?, ?, true)",
        [(sid, sym, day, px, px, dt.datetime.combine(day, dt.time(22)), day) for sid, sym, day, px in rows],
    )
    options = DelistingEvidenceOptions(run_id="p2")
    refresh_delisting_evidence(store, options)
    assert fold_evidence_into_delisting_events(store, options) == 1


def _terminals(store):
    return store.con.execute(
        "SELECT delist_date, terminal_return, terminal_return_source FROM delisting_terminal_returns ORDER BY 1"
    ).fetchall()


def test_observed_dlret_on_dlstdt_basis_is_one_terminal_that_keeps_the_last_session(tmp_store):
    # Review probe P2: a CRSP-style row keyed on DLSTDT = last trade date, landed directly.
    from atx_db.calendar import TradingCalendarDataset, TradingCalendarOptions
    from atx_db.delisting import refresh_delisting_terminal_returns, refresh_survivorship_safe_forward_returns

    _seed_observed_cessation(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO delisting_return_observations (delisting_return_observation_id, source, provider, "
        "security_id, symbol, delist_date, as_of_date, available_at, delisting_return, return_basis) VALUES "
        "('obs-1','crsp','CRSP','SEC-OBS','OBS',?,?,?,-0.9,'CRSP_DLRET')",
        [OBS_SESSIONS[LAST], OBS_SESSIONS[LAST], dt.datetime.combine(OBS_SESSIONS[LAST + 5], dt.time(12))],
    )
    assert refresh_delisting_terminal_returns(tmp_store) == 1
    assert _terminals(tmp_store) == [(OBS_SESSIONS[LAST + 1], -0.9, "observed")]

    TradingCalendarDataset().load(tmp_store, TradingCalendarOptions())
    refresh_survivorship_safe_forward_returns(tmp_store)

    def label(index, horizon):
        return tmp_store.con.execute(
            "SELECT raw_forward_return, forward_return, is_stitched FROM forward_returns_survivorship_safe "
            "WHERE security_id = 'SEC-OBS' AND as_of_date = ? AND horizon_days = ?",
            [OBS_SESSIONS[index], horizon],
        ).fetchone()

    # A window spanning the delisting keeps the +20% final session: (12 / 10) * (1 - 0.9) - 1.
    # (On the DLSTDT basis the leg ended at the prior close: raw 0, label -0.9.)
    assert label(LAST - 1, 5)[0] == pytest.approx(0.2)
    assert label(LAST - 1, 5)[1] == pytest.approx(1.2 * 0.1 - 1)
    assert label(LAST - 1, 1) == pytest.approx((0.2, 0.2, False))  # ends on the last trade: a survivor
    assert label(LAST, 1)[1] == pytest.approx(-0.9)  # formation on the last trade day: pure terminal


def test_observation_load_moves_dlstdt_to_the_next_observed_session(tmp_store, tmp_path):
    from atx_db.delisting import (
        DelistingReturnObservationOptions,
        load_delisting_return_observations,
        refresh_delisting_terminal_returns,
    )

    _seed_observed_cessation(tmp_store)
    csv_path = tmp_path / "dlret.csv"
    csv_path.write_text(
        "PERMNO,security_id,TICKER,DLSTDT,DLSTCD,DLRET,available_at\n"
        f"77,SEC-OBS,OBS,{OBS_SESSIONS[LAST].isoformat()},560,-0.9,{OBS_SESSIONS[LAST + 5].isoformat()}T12:00:00\n",
        encoding="utf-8",
    )
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE")
    )
    stored, raw = tmp_store.con.execute(
        "SELECT delist_date, json_extract_string(raw_payload_json, '$.delist_date') FROM delisting_return_observations"
    ).fetchone()
    assert stored == OBS_SESSIONS[LAST + 1]
    assert raw == OBS_SESSIONS[LAST].isoformat()  # the vendor's DLSTDT is retained
    assert refresh_delisting_terminal_returns(tmp_store) == 1
    assert _terminals(tmp_store) == [(OBS_SESSIONS[LAST + 1], -0.9, "observed")]
    assert (
        tmp_store.con.execute(
            "SELECT count(*) FROM delisting_events e JOIN delisting_terminal_returns t "
            "ON t.security_id = e.security_id AND t.delist_date = e.delist_date"
        ).fetchone()[0]
        == 1
    )


def test_vendor_and_archive_last_trade_disagreement_still_yields_one_terminal(tmp_store):
    from atx_db.delisting import refresh_delisting_terminal_returns

    _seed_observed_cessation(tmp_store)
    # Vendor says the exchange price ended two sessions before the archive's last bar.
    tmp_store.con.execute(
        "INSERT INTO delisting_return_observations (delisting_return_observation_id, source, provider, "
        "security_id, symbol, delist_date, as_of_date, available_at, delisting_return, return_basis) VALUES "
        "('obs-2','crsp','CRSP','SEC-OBS','OBS',?,?,?,-0.8,'CRSP_DLRET')",
        [OBS_SESSIONS[LAST - 1], OBS_SESSIONS[LAST - 1], dt.datetime.combine(OBS_SESSIONS[LAST + 5], dt.time(12))],
    )
    assert refresh_delisting_terminal_returns(tmp_store) == 1  # no Shumway row beside the observed one
    assert _terminals(tmp_store) == [(OBS_SESSIONS[LAST - 1], -0.8, "observed")]
