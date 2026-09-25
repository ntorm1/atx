from __future__ import annotations

import datetime as dt
from collections.abc import Iterator

import duckdb
import pytest

from atx_db.connection import DuckDBStore
from atx_db.delisting import (
    DEFAULT_FORWARD_RETURN_SS_SOURCE,
    FORWARD_RETURN_SS_COLUMNS,
    SurvivorshipSafeForwardReturnOptions,
    _forward_return_id,
    refresh_survivorship_safe_forward_returns,
    survivorship_forward_return_diagnostics,
)
from atx_db.quality.checks_survivorship import (
    _SURVIVORSHIP_HORIZONS,
    survivorship_forward_return_check,
)
from atx_db.signal_eval import IC_HORIZONS


@pytest.fixture
def panel_store() -> Iterator[DuckDBStore]:
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(config={"threads": 1, "memory_limit": "128MB"})
    store._initialized = True
    store.con.execute("SET preserve_insertion_order = false")
    store.con.execute(
        """
        CREATE TABLE equity_daily_bars (
            source VARCHAR NOT NULL, security_id VARCHAR NOT NULL,
            symbol VARCHAR NOT NULL, trade_date DATE NOT NULL,
            close DOUBLE, adjusted_close DOUBLE, available_at TIMESTAMP,
            vendor_security_id VARCHAR, source_loaded_at TIMESTAMP
        );
        CREATE TABLE trading_calendar (
            calendar_id VARCHAR, trade_date DATE, is_open BOOLEAN, source VARCHAR
        );
        CREATE TABLE delisting_terminal_returns (
            terminal_return_id VARCHAR PRIMARY KEY, source VARCHAR,
            security_id VARCHAR, delist_date DATE, terminal_return DOUBLE,
            terminal_return_source VARCHAR, return_observation_id VARCHAR,
            available_at TIMESTAMP, source_loaded_at TIMESTAMP,
            is_latest_revision BOOLEAN, terminal_return_policy VARCHAR
        );
        CREATE TABLE delisting_events (
            security_id VARCHAR, delist_date DATE, available_at TIMESTAMP,
            delisting_event_id VARCHAR, delist_reason VARCHAR, details_json VARCHAR
        );
        CREATE TABLE forward_returns_survivorship_safe (
            forward_return_id VARCHAR PRIMARY KEY, source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL, symbol VARCHAR, as_of_date DATE NOT NULL,
            horizon_days INTEGER NOT NULL, forward_end_date DATE,
            raw_forward_return DOUBLE, terminal_return DOUBLE,
            forward_return DOUBLE NOT NULL, is_delisted_in_horizon BOOLEAN NOT NULL,
            is_stitched BOOLEAN NOT NULL, delist_date DATE,
            terminal_return_source VARCHAR, return_observation_id VARCHAR,
            is_latest_revision BOOLEAN, available_at TIMESTAMP NOT NULL,
            run_id VARCHAR CHECK (run_id IS NULL OR run_id <> '__reject__'),
            price_basis VARCHAR, calculation_version VARCHAR
        )
        """
    )
    store.con.executemany(
        "INSERT INTO trading_calendar VALUES ('XNYS', ?, true, 'equity_daily_bars calendar')",
        [(dt.date(2024, 1, day),) for day in range(1, 15)],
    )
    try:
        yield store
    finally:
        store.connection.close()


def _bar(store, security, day, adjusted, *, raw=None, available=None, source="bars"):
    store.con.execute(
        "INSERT INTO equity_daily_bars VALUES (?, ?, ?, ?, ?, ?, ?, 'vendor', '2024-02-01')",
        [source, security, f"{security}-symbol", dt.date(2024, 1, day),
         adjusted if raw is None else raw, adjusted,
         available or dt.datetime(2024, 1, day, 22)],
    )


def _terminal(store, *, identifier="terminal", day=4, value=-0.5,
              available=dt.datetime(2024, 1, 9), latest=True, security="D",
              source="observed", policy=None):
    store.con.execute(
        "INSERT INTO delisting_terminal_returns (terminal_return_id, source, security_id, delist_date, "
        "terminal_return, terminal_return_source, return_observation_id, available_at, source_loaded_at, "
        "is_latest_revision, terminal_return_policy) VALUES "
        "(?, 'terminal-source', ?, ?, ?, ?, ?, ?, '2024-02-01', ?, ?)",
        [identifier, security, dt.date(2024, 1, day), value, source, f"obs-{identifier}", available, latest,
         policy],
    )


def _row(store, security="D", day=1, horizon=5, source=DEFAULT_FORWARD_RETURN_SS_SOURCE):
    result = store.con.execute(
        f"SELECT {', '.join(FORWARD_RETURN_SS_COLUMNS)} FROM forward_returns_survivorship_safe "
        "WHERE source = ? AND security_id = ? AND as_of_date = ? AND horizon_days = ?",
        [source, security, dt.date(2024, 1, day), horizon],
    ).fetchone()
    return None if result is None else dict(zip(FORWARD_RETURN_SS_COLUMNS, result, strict=True))


def test_nonzero_adjusted_preterminal_leg_all_metadata_and_max_clocks(panel_store):
    _bar(panel_store, "D", 1, 100, raw=100, available=dt.datetime(2024, 1, 10, 12))
    _bar(panel_store, "D", 3, 120, raw=60)
    _bar(panel_store, "D", 4, 999, available=dt.datetime(2025, 1, 1))
    _bar(panel_store, "D", 6, 9999)
    _terminal(panel_store)
    _terminal(panel_store, identifier="second", day=5, value=-0.9)
    assert refresh_survivorship_safe_forward_returns(panel_store) > 0
    row = _row(panel_store)
    assert row["raw_forward_return"] == pytest.approx(0.2)
    assert row["forward_return"] == pytest.approx(-0.4)
    assert row["terminal_return"] == -0.5
    assert row["symbol"] == "D-symbol"
    assert row["forward_end_date"] == dt.date(2024, 1, 6)
    assert row["available_at"] == dt.datetime(2024, 1, 10, 12)
    assert row["return_observation_id"] == "obs-terminal"
    assert row["terminal_return_source"] == "observed"
    assert row["is_stitched"] and row["is_delisted_in_horizon"]
    assert row["forward_return_id"] == _forward_return_id(
        DEFAULT_FORWARD_RETURN_SS_SOURCE, "D", dt.date(2024, 1, 1), 5,
    )
    assert _row(panel_store, day=4) is None  # no post-terminal formation resurrection
    assert survivorship_forward_return_check(panel_store).status == "passed"


def test_calendar_endpoint_never_shifts_to_next_security_observation(panel_store):
    _bar(panel_store, "S", 1, 100, available=dt.datetime(2024, 1, 12))
    _bar(panel_store, "S", 2, 110)
    _bar(panel_store, "S", 7, 130)  # day 6 (five observed sessions ahead) is missing
    refresh_survivorship_safe_forward_returns(panel_store)
    assert _row(panel_store, "S", horizon=5) is None
    row = _row(panel_store, "S", horizon=1)
    assert row["forward_end_date"] == dt.date(2024, 1, 2)
    assert row["forward_return"] == pytest.approx(0.1)
    assert row["available_at"] == dt.datetime(2024, 1, 12)


def test_formation_only_price_keeps_pure_terminal_row_and_clock(panel_store):
    _bar(panel_store, "D", 1, 100)
    _terminal(panel_store)
    refresh_survivorship_safe_forward_returns(panel_store)
    row = _row(panel_store)
    assert row["raw_forward_return"] == 0
    assert row["forward_return"] == -0.5
    assert row["symbol"] == "D-symbol"
    assert row["forward_end_date"] == dt.date(2024, 1, 6)
    assert row["available_at"] == dt.datetime(2024, 1, 9)


def test_cutoff_selects_old_eligible_terminal_and_price_before_latest_flags(panel_store):
    _bar(panel_store, "D", 1, 100)
    _bar(panel_store, "D", 1, 500, available=dt.datetime(2024, 1, 12))
    _bar(panel_store, "D", 3, 120)
    _terminal(panel_store, latest=False, available=dt.datetime(2024, 1, 7))
    _terminal(panel_store, identifier="revision", value=-0.9,
              available=dt.datetime(2024, 1, 12))
    cutoff = dt.datetime(2024, 1, 10, 12, tzinfo=dt.UTC)
    options = SurvivorshipSafeForwardReturnOptions(observation_cutoff=cutoff)
    refresh_survivorship_safe_forward_returns(panel_store, options)
    assert _row(panel_store)["forward_return"] == pytest.approx(-0.4)
    assert _row(panel_store)["return_observation_id"] == "obs-terminal"
    assert _row(panel_store, horizon=10) is None  # calendar endpoint beyond vintage
    assert survivorship_forward_return_check(
        panel_store, observation_cutoff=cutoff,
    ).status == "passed"
    refresh_survivorship_safe_forward_returns(panel_store)
    assert _row(panel_store)["forward_return"] == pytest.approx(-0.976)


def test_late_terminal_not_admitted_before_confirmation(panel_store):
    _bar(panel_store, "D", 1, 100)
    _terminal(panel_store, available=dt.datetime(2024, 1, 12))
    cutoff = dt.datetime(2024, 1, 10, 22)
    assert refresh_survivorship_safe_forward_returns(
        panel_store, SurvivorshipSafeForwardReturnOptions(observation_cutoff=cutoff),
    ) == 0
    assert survivorship_forward_return_check(
        panel_store, observation_cutoff=cutoff,
    ).observed_value == 0


def test_duplicate_price_sources_have_deterministic_no_fanout_selection(panel_store):
    _bar(panel_store, "S", 1, 200, source="z-source")
    _bar(panel_store, "S", 1, 100, source="a-source")
    _bar(panel_store, "S", 2, 110)
    first_count = refresh_survivorship_safe_forward_returns(panel_store)
    assert _row(panel_store, "S", horizon=1)["forward_return"] == pytest.approx(0.1)
    panel_store.con.execute("CREATE TEMP TABLE saved AS SELECT * FROM equity_daily_bars")
    panel_store.con.execute("DELETE FROM equity_daily_bars")
    panel_store.con.execute("INSERT INTO equity_daily_bars SELECT * FROM saved ORDER BY source")
    assert refresh_survivorship_safe_forward_returns(panel_store) == first_count
    assert _row(panel_store, "S", horizon=1)["forward_return"] == pytest.approx(0.1)


def test_source_scoped_idempotence_and_empty_refresh(panel_store):
    _bar(panel_store, "D", 1, 100)
    _terminal(panel_store)
    first = refresh_survivorship_safe_forward_returns(panel_store)
    original = panel_store.con.execute(
        "SELECT * FROM forward_returns_survivorship_safe ORDER BY forward_return_id",
    ).fetchall()
    assert refresh_survivorship_safe_forward_returns(panel_store) == first
    assert panel_store.con.execute(
        "SELECT * FROM forward_returns_survivorship_safe ORDER BY forward_return_id",
    ).fetchall() == original
    assert refresh_survivorship_safe_forward_returns(
        panel_store, SurvivorshipSafeForwardReturnOptions(source="other"),
    ) == first
    panel_store.con.execute("DELETE FROM equity_daily_bars")
    assert refresh_survivorship_safe_forward_returns(panel_store) == 0
    assert panel_store.con.execute(
        "SELECT count(*) FROM forward_returns_survivorship_safe WHERE source='other'",
    ).fetchone()[0] == first


def test_failed_publication_rolls_back_source_delete_and_cleans_temporary_tables(panel_store):
    _bar(panel_store, "D", 1, 100)
    _terminal(panel_store)
    refresh_survivorship_safe_forward_returns(panel_store)
    previous = panel_store.con.execute(
        "SELECT * FROM forward_returns_survivorship_safe ORDER BY forward_return_id",
    ).fetchall()
    with pytest.raises(duckdb.ConstraintException):
        refresh_survivorship_safe_forward_returns(
            panel_store, SurvivorshipSafeForwardReturnOptions(run_id="__reject__"),
        )
    assert panel_store.con.execute(
        "SELECT * FROM forward_returns_survivorship_safe ORDER BY forward_return_id",
    ).fetchall() == previous
    assert panel_store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name LIKE '_ss_%'",
    ).fetchone()[0] == 0


def test_empty_output_fails_input_derived_gate_and_other_source_cannot_mask_it(panel_store):
    _bar(panel_store, "D", 1, 100)
    _terminal(panel_store)
    result = survivorship_forward_return_check(panel_store)
    assert result.status == "failed" and result.observed_value == 2
    refresh_survivorship_safe_forward_returns(
        panel_store, SurvivorshipSafeForwardReturnOptions(source="other"),
    )
    assert survivorship_forward_return_check(panel_store).status == "failed"
    refresh_survivorship_safe_forward_returns(panel_store)
    assert survivorship_forward_return_check(panel_store).status == "passed"


def test_halted_name_has_no_demand_on_other_security_formation_dates(panel_store):
    _bar(panel_store, "D", 1, 100)
    _bar(panel_store, "S", 2, 100)
    _terminal(panel_store)
    refresh_survivorship_safe_forward_returns(panel_store)
    assert _row(panel_store, day=2) is None
    assert survivorship_forward_return_check(panel_store).status == "passed"


def test_missing_adjustment_is_not_raw_fallback_or_vacuous_quality_pass(panel_store):
    _bar(panel_store, "D", 1, None, raw=100)
    _terminal(panel_store)
    panel_store.con.execute(
        "INSERT INTO delisting_events (security_id, delist_date, available_at) "
        "VALUES ('U', '2024-01-02', '2024-01-03')",
    )
    assert refresh_survivorship_safe_forward_returns(panel_store) == 0
    assert survivorship_forward_return_check(panel_store).status == "failed"
    report = survivorship_forward_return_diagnostics(panel_store)
    assert report["missing_adjusted_price_source_rows"] == 1
    assert report["stitched_rows"] == 0
    assert report["uncovered_event_rows"] == 1
    options = SurvivorshipSafeForwardReturnOptions(source="raw-compat", price_basis="close")
    assert refresh_survivorship_safe_forward_returns(panel_store, options) == 2
    assert survivorship_forward_return_check(
        panel_store, source="raw-compat", price_basis="close",
    ).status == "passed"


def test_quality_horizon_contract_and_bad_price_basis(panel_store):
    assert _SURVIVORSHIP_HORIZONS == IC_HORIZONS
    with pytest.raises(ValueError, match="price_basis"):
        refresh_survivorship_safe_forward_returns(
            panel_store, SurvivorshipSafeForwardReturnOptions(price_basis="split_factor"),
        )


def test_missing_bar_clock_uses_modeled_22h_and_late_preterminal_clock_is_preserved(panel_store):
    _bar(panel_store, "D", 1, 100)
    panel_store.con.execute("UPDATE equity_daily_bars SET available_at=NULL")
    _bar(panel_store, "D", 3, 120, available=dt.datetime(2024, 1, 11, 12))
    _terminal(panel_store)
    refresh_survivorship_safe_forward_returns(panel_store)
    assert _row(panel_store)["available_at"] == dt.datetime(2024, 1, 11, 12)
    panel_store.con.execute("DELETE FROM equity_daily_bars WHERE trade_date='2024-01-03'")
    _bar(panel_store, "D", 2, 110)
    refresh_survivorship_safe_forward_returns(panel_store)
    assert _row(panel_store, horizon=1)["available_at"] == dt.datetime(2024, 1, 2, 22)


@pytest.mark.parametrize("invalid", [-1.01, float("inf"), float("nan")])
def test_invalid_terminal_correction_never_resurrects_older_revision(panel_store, invalid):
    for day, price in [(1, 100), (2, 110), (3, 120), (5, 90), (6, 80), (7, 70)]:
        _bar(panel_store, "D", day, price)
    _terminal(panel_store, latest=False, available=dt.datetime(2024, 1, 7))
    _terminal(panel_store, identifier="invalid-correction", value=invalid,
              available=dt.datetime(2024, 1, 12))
    cutoff = dt.datetime(2024, 1, 10, 22)
    refresh_survivorship_safe_forward_returns(
        panel_store, SurvivorshipSafeForwardReturnOptions(observation_cutoff=cutoff),
    )
    assert _row(panel_store)["forward_return"] == pytest.approx(-0.4)
    assert _row(panel_store)["return_observation_id"] == "obs-terminal"
    assert survivorship_forward_return_check(
        panel_store, observation_cutoff=cutoff,
    ).status == "passed"
    # Even an existing valid-looking output cannot mask an invalid selected input.
    before_refresh = survivorship_forward_return_check(panel_store)
    assert before_refresh.status == "failed" and before_refresh.observed_value == 1

    refresh_survivorship_safe_forward_returns(panel_store)
    assert _row(panel_store) is None
    assert _row(panel_store, day=6, horizon=1) is None
    assert _row(panel_store, horizon=1)["forward_return"] == pytest.approx(0.1)
    result = survivorship_forward_return_check(panel_store)
    assert result.status == "failed" and result.observed_value == 1


@pytest.mark.parametrize("invalid", [-1.01, float("inf"), float("nan")])
def test_invalid_only_terminal_blocks_survivor_and_later_event_fallback(panel_store, invalid):
    for day, price in [(1, 100), (3, 120), (6, 80), (7, 70), (10, 60)]:
        _bar(panel_store, "D", day, price)
    _terminal(panel_store, identifier="invalid-only", value=invalid,
              available=dt.datetime(2024, 1, 12))
    cutoff = dt.datetime(2024, 1, 10, 22)
    refresh_survivorship_safe_forward_returns(
        panel_store, SurvivorshipSafeForwardReturnOptions(observation_cutoff=cutoff),
    )
    assert _row(panel_store)["forward_return"] == pytest.approx(-0.2)
    assert survivorship_forward_return_check(
        panel_store, observation_cutoff=cutoff,
    ).status == "passed"
    # A valid later event does not replace the first known, invalid event boundary.
    _terminal(panel_store, identifier="later-event", day=8, value=-0.6,
              available=dt.datetime(2024, 1, 13))
    refresh_survivorship_safe_forward_returns(panel_store)
    assert _row(panel_store) is None
    assert _row(panel_store, day=6, horizon=1) is None
    result = survivorship_forward_return_check(panel_store)
    assert result.status == "failed" and result.observed_value == 1


def test_invalid_selected_terminal_fails_quality_without_any_formation_input(panel_store):
    _terminal(panel_store, value=float("nan"))
    assert refresh_survivorship_safe_forward_returns(panel_store) == 0
    result = survivorship_forward_return_check(panel_store)
    assert result.status == "failed" and result.observed_value == 1


def test_last_observed_trade_basis_keeps_the_final_session_return(panel_store):
    # A3 delist_date basis: the first session after the last observed trade. The pre-terminal
    # leg ends at the last traded close, and a formation on the last trade day still earns
    # the terminal return -- no final-session return is dropped from the label.
    _bar(panel_store, "D", 1, 100)
    _bar(panel_store, "D", 2, 110)
    _bar(panel_store, "D", 3, 99)  # last observed trade
    _terminal(panel_store, day=4, value=-0.3, available=dt.datetime(2024, 1, 4, 22))
    refresh_survivorship_safe_forward_returns(panel_store)
    last_day = _row(panel_store, day=3, horizon=1)
    assert last_day["raw_forward_return"] == 0
    assert last_day["forward_return"] == pytest.approx(-0.3)
    assert last_day["delist_date"] == dt.date(2024, 1, 4)
    full = _row(panel_store, day=1, horizon=5)
    assert full["raw_forward_return"] == pytest.approx(-0.01)  # 99 / 100 - 1, through the last trade
    assert full["forward_return"] == pytest.approx(0.99 * 0.7 - 1)
    assert _row(panel_store, day=4, horizon=1) is None


def test_diagnostics_publish_rx2_policy_bias_exposure(panel_store):
    from atx_db.delisting import PERFORMANCE_TERMINAL_RETURN_POLICY_CODE

    _terminal(panel_store, identifier="orphan", security="U", day=4, value=-0.3, source="policy",
              policy=PERFORMANCE_TERMINAL_RETURN_POLICY_CODE, available=dt.datetime(2024, 1, 4, 22))
    _terminal(panel_store, identifier="linked", security="L", day=4, value=-0.3, source="policy",
              policy=PERFORMANCE_TERMINAL_RETURN_POLICY_CODE, available=dt.datetime(2024, 1, 4, 22))
    _terminal(panel_store, identifier="late", security="Z", day=4, value=-0.3, source="policy",
              policy=PERFORMANCE_TERMINAL_RETURN_POLICY_CODE, available=dt.datetime(2024, 1, 12))
    panel_store.con.execute(
        "INSERT INTO delisting_events (security_id, delist_date, available_at, delisting_event_id, "
        "delist_reason, details_json) VALUES "
        "('U', '2024-01-04', '2024-01-04 22:00', 'e-u', 'unknown', '{\"cik\": null}'),"
        "('L', '2024-01-04', '2024-01-04 22:00', 'e-l', 'unknown', '{\"cik\": \"0000000001\"}'),"
        "('Z', '2024-01-04', '2024-01-04 22:00', 'e-z', 'unknown', '{}')"
    )
    options = SurvivorshipSafeForwardReturnOptions(observation_cutoff=dt.datetime(2024, 1, 10, 22))
    exposure = survivorship_forward_return_diagnostics(panel_store, options)["policy_bias_exposure"]
    # Z's terminal is not yet visible at the cutoff; L is linked to a CIK.
    assert exposure["performance_policy_rows"] == 2
    assert exposure["cik_less_unknown_performance_policy_rows"] == 1
    assert exposure["cik_less_unknown_share_of_terminal_rows"] == pytest.approx(0.5)
    assert survivorship_forward_return_diagnostics(panel_store)["policy_bias_exposure"][
        "cik_less_unknown_performance_policy_rows"
    ] == 2
