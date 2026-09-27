from __future__ import annotations

import datetime as dt
from types import SimpleNamespace

import duckdb
import pytest

from atx_db.signal_eval import load_panel_for_eval


def _panel_store() -> duckdb.DuckDBPyConnection:
    con = duckdb.connect(":memory:", config={"memory_limit": "256MB", "threads": 1})
    for table in ("fundamental_factor_values", "cross_domain_factor_values"):
        con.execute(
            f"""
            CREATE TABLE {table} (
                security_id VARCHAR,as_of_date DATE,factor_id VARCHAR,value DOUBLE,
                available_at TIMESTAMP,source_loaded_at TIMESTAMP,run_id VARCHAR,
                input_lineage_json VARCHAR,is_latest_revision BOOLEAN,source VARCHAR,
                factor_value_id VARCHAR
            )
            """
        )
    con.execute(
        """
        CREATE TABLE universe_membership (
            universe_id VARCHAR,security_id VARCHAR,valid_from DATE,valid_to DATE,
            as_of_date DATE,is_member BOOLEAN,is_latest_revision BOOLEAN,
            available_at TIMESTAMP,source_loaded_at TIMESTAMP,source VARCHAR
        )
        """
    )
    for security_id in ("S1", "S2", "S3"):
        con.execute(
            "INSERT INTO universe_membership VALUES (?,?,?,?,?,true,true,?,?,?)",
            [
                "us_common_equity_liquid_v1",
                security_id,
                dt.date(2020, 1, 1),
                None,
                dt.date(2020, 1, 1),
                dt.datetime(2020, 1, 1, 20),
                dt.datetime(2020, 1, 1, 21),
                "fixture",
            ],
        )
    return con


def _insert(con, table, security_id, as_of_date, factor_id, value, clock, is_latest, source) -> None:
    con.execute(
        f"INSERT INTO {table} VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        [
            security_id,
            as_of_date,
            factor_id,
            value,
            clock,
            clock,
            "run",
            "{}",
            is_latest,
            source,
            f"{source}|{factor_id}|{security_id}|{as_of_date}",
        ],
    )


def test_load_panel_for_eval_filters_base_tables_before_pit_windows() -> None:
    con = _panel_store()
    try:
        clock = dt.datetime(2020, 1, 31, 20)
        _insert(con, "fundamental_factor_values", "S1", dt.date(2020, 1, 31), "wanted", 1.5, clock, True, "src")
        _insert(con, "cross_domain_factor_values", "S2", dt.date(2020, 1, 31), "other", 1.5, clock, True, "src")

        panel = load_panel_for_eval(SimpleNamespace(con=con), factor_ids=["wanted"])

        assert len(panel) == 1
        row = panel.iloc[0]
        assert row["security_id"] == "S1"
        assert row["as_of_date"].date() == dt.date(2020, 1, 31)
        assert row["factor_id"] == "wanted"
        assert row["value"] == 1.5
        assert row["available_at"].to_pydatetime() == dt.datetime(2020, 1, 31, 20)
    finally:
        con.close()


def test_load_panel_for_eval_never_resurrects_retired_legacy_rows() -> None:
    # derived_factor_projection retires the legacy source's rows of a projected factor
    # (is_latest_revision=false on a one-row-per-key table): a retirement marker, not an older
    # revision, so a retired row never re-enters the panel (tier-1 v2 node 2.2, review I1).
    con = _panel_store()
    try:
        jan15, jan31 = dt.date(2020, 1, 15), dt.date(2020, 1, 31)
        table = "fundamental_factor_values"
        # S1: legacy only, retired (the projection's eligibility filter excluded S1).
        _insert(con, table, "S1", jan31, "F", 9.9, dt.datetime(2020, 1, 31, 20), False, "legacy")
        # S2: retired legacy row with a LATER clock than the projection row of the same key.
        _insert(con, table, "S2", jan31, "F", 7.7, dt.datetime(2020, 1, 31, 21), False, "legacy")
        _insert(con, table, "S2", jan31, "F", 1.0, dt.datetime(2020, 1, 31, 20), True, "projection")
        # S3: retired legacy row on an off-grid date the projection never emits.
        _insert(con, table, "S3", jan15, "F", 5.5, dt.datetime(2020, 1, 15, 20), False, "legacy")
        _insert(con, table, "S3", jan31, "F", 2.0, dt.datetime(2020, 1, 31, 20), True, "projection")

        panel = load_panel_for_eval(SimpleNamespace(con=con), factor_ids=["F"])

        got = [
            (row.security_id, row.as_of_date.date(), row.value)
            for row in panel.itertuples(index=False)
        ]
        assert got == [("S2", jan31, 1.0), ("S3", jan31, 2.0)]
    finally:
        con.close()


@pytest.mark.parametrize("reverse", [False, True])
def test_live_factor_candidates_survive_membership_pick_until_revision_rank(reverse) -> None:
    con = _panel_store()
    try:
        day = dt.date(2020, 1, 31)
        early, late = dt.datetime(2020, 1, 31, 20), dt.datetime(2020, 1, 31, 21)
        rows = [("S1", day, 1.0, early, "a"), ("S1", day, 2.0, early, "z"),
                ("S2", day, 7.0, early, "a"), ("S2", day, None, late, "z"),
                ("S3", dt.date(2020, 1, 15), 3.0, early, "same"),
                ("S3", day, 4.0, early, "same")]
        for security, formation, value, clock, source in reversed(rows) if reverse else rows:
            _insert(con, "fundamental_factor_values", security, formation, "F", value, clock, True, source)
        panel = load_panel_for_eval(SimpleNamespace(con=con), factor_ids=["F"])
        # Source and row-id tie-breaks survive the membership window. The newest NULL for
        # S2 wins whole and is then omitted; the earlier non-NULL must not reappear.
        assert [(r.security_id, r.as_of_date.date(), r.value) for r in panel.itertuples()] == [
            ("S1", day, 2.0), ("S3", day, 4.0)]
    finally:
        con.close()


def test_latest_visible_nonmember_does_not_revive_older_membership() -> None:
    con = _panel_store()
    try:
        day, clock = dt.date(2020, 1, 31), dt.datetime(2020, 1, 31, 20)
        _insert(con, "fundamental_factor_values", "S1", day, "F", 1.0, clock, True, "projection")
        con.execute("""
            INSERT INTO universe_membership VALUES
            ('us_common_equity_liquid_v1', 'S1', DATE '2020-01-15', NULL, DATE '2020-01-15',
             false, true, TIMESTAMP '2020-01-15 20:00:00', TIMESTAMP '2020-01-15 21:00:00', 'new')
        """)
        assert load_panel_for_eval(SimpleNamespace(con=con), factor_ids=["F"]).empty
    finally:
        con.close()
