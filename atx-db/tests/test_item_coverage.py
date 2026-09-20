"""Annual cohort selection, bounded SQL measurement and non-vacuous coverage gates."""

from __future__ import annotations

import datetime as dt
from dataclasses import replace

import pandas as pd
import pytest

from atx_db.item_coverage import (
    DEFAULT_SOURCE,
    DEFAULT_UNIVERSE_ID,
    ITEM_COVERAGE_COLUMNS,
    ItemCoverageOptions,
    compute_item_coverage_rows,
    evaluate_item_coverage_gate,
    measure_item_coverage,
    refresh_item_coverage,
    render_item_coverage_markdown,
)
from atx_db.item_coverage_cohort import AnnualCoverageCohortOptions, refresh_item_coverage_cohort
from atx_db.provider_coverage import coverage_gate_item_count

AS_OF = dt.date(2022, 6, 1)
OPTIONS = ItemCoverageOptions(
    as_of_date=AS_OF, minimum_fiscal_year=2020, maximum_fiscal_year=2022, bases=("annual",), item_ids=(1001, 1101)
)


@pytest.fixture
def coverage_store(tmp_store):
    tmp_store.con.execute(
        "INSERT INTO fundamental_item (item_id,canonical_code,statement,section,data_type,"
        "unit_type,sign_convention,is_derived,definition) VALUES "
        "(1001,'revenue','income','revenue','monetary','USD','positive',false,'test'),"
        "(1101,'total_assets','balance','assets','monetary','USD','positive',false,'test')"
    )
    return tmp_store


def _market_and_listing(store, *, n=1, year=2020):
    store.con.execute(
        """INSERT INTO market_daily_metrics
        (market_daily_id,source,security_id,trade_date,market_cap,available_at,inputs_hash,as_of_date)
        SELECT 'm-'||?||'-'||i,'atx-db daily market panel v1','S'||lpad(i::VARCHAR,5,'0'),
               make_date(?,12,31),1000,make_date(?,12,31)+INTERVAL 20 HOUR,'h',make_date(?,12,31)
        FROM range(?) r(i)""",
        [year, year, year, year, n],
    )
    store.con.execute(
        """INSERT INTO universe_us_listed_membership
        (membership_id,universe_id,security_id,valid_from,valid_to,available_at,security_type,
         exchange_code,has_cik,reason,rules_json,decision_count,as_of_date,source)
        SELECT 'l-'||?||'-'||i,'us_listed_v1','S'||lpad(i::VARCHAR,5,'0'),make_date(?,1,1),
               make_date(?+1,1,1),make_date(?,1,1),'common','XNYS',true,'member','{}',1,
               make_date(?,1,1),'atx-db us-listed universe builder' FROM range(?) r(i)""",
        [year, year, year, year, year, n],
    )


def _fact(store, security, year, *, item=1001, value=10.0, available=None, key=None, latest=True):
    store.con.execute(
        """INSERT INTO fundamental_standardized
        (standardized_id,source,upstream_source,security_id,cik,item_id,canonical_code,basis,
         period_end,fiscal_year,value,available_at,as_of_date,input_codes_json,input_item_ids_json,
         rule_id,combination_rule,is_latest_revision,source_accession)
        VALUES (?,?,'test',?,'1',?,?,'annual',make_date(?,12,31),?,?,?,make_date(?,12,31),
                '[]','[]','r','identity',?,'accession')""",
        [
            key or f"{security}-{year}-{item}",
            DEFAULT_SOURCE,
            security,
            item,
            "revenue" if item == 1001 else "total_assets",
            year,
            year,
            value,
            available or dt.datetime(year + 1, 2, 1),
            year,
            latest,
        ],
    )


def _cohort(store, *, minimum=2020, maximum=2022):
    return refresh_item_coverage_cohort(
        store, AnnualCoverageCohortOptions(as_of_date=AS_OF, minimum_fiscal_year=minimum, maximum_fiscal_year=maximum)
    )


def test_pure_same_year_membership_rejected_member_and_zero_not_null():
    members = pd.DataFrame(
        [("A", 2020, True), ("A", 2020, True), ("B", 2021, True), ("C", 2020, False), ("D", 2020, True)],
        columns=["security_id", "fiscal_year", "is_member"],
    )
    facts = pd.DataFrame(
        [
            ("A", 1001, "revenue", "annual", 2020, 0.0),
            ("B", 1001, "revenue", "annual", 2020, 1.0),
            ("C", 1001, "revenue", "annual", 2020, 1.0),
            ("D", 1001, "revenue", "annual", 2020, None),
        ],
        columns=["security_id", "item_id", "canonical_code", "basis", "fiscal_year", "value"],
    )
    registry = pd.DataFrame([(1001, "revenue"), (1101, "total_assets")], columns=["item_id", "canonical_code"])
    rows = compute_item_coverage_rows(facts, members, OPTIONS, registry=registry)
    revenue = rows[(rows.item_id == 1001) & (rows.fiscal_year == 2020)].iloc[0]
    assert (revenue.n_securities, revenue.n_with_value, revenue.coverage_pct) == (2, 1, 50.0)
    assert len(rows) == 6  # Includes absent FY2022 and wholly absent registered item.
    assert rows.loc[rows.fiscal_year == 2022, "n_securities"].eq(0).all()
    assert rows.loc[rows.item_id == 1101, "n_with_value"].eq(0).all()
    assert rows.coverage_pct.le(100).all()
    assert tuple(rows.columns) == ITEM_COVERAGE_COLUMNS


def test_exact_top_3000_tie_pit_and_half_open_boundary(coverage_store):
    _market_and_listing(coverage_store, n=3005)
    # Large caps with unusable metadata cannot displace a known eligible security.
    coverage_store.con.execute("UPDATE market_daily_metrics SET market_cap=99999 WHERE security_id>='S03001'")
    coverage_store.con.execute(
        "UPDATE universe_us_listed_membership SET available_at=TIMESTAMP '2021-01-01' WHERE security_id='S03001'"
    )
    coverage_store.con.execute(
        "UPDATE universe_us_listed_membership SET valid_to=DATE '2020-12-31' WHERE security_id='S03002'"
    )
    coverage_store.con.execute("UPDATE universe_us_listed_membership SET security_type='ETF' WHERE security_id='S03003'")
    coverage_store.con.execute("UPDATE market_daily_metrics SET market_cap=0 WHERE security_id='S03004'")
    _cohort(coverage_store)
    members = coverage_store.con.execute(
        "SELECT security_id,market_cap_rank FROM item_coverage_annual_cohort ORDER BY market_cap_rank"
    ).fetchall()
    assert len(members) == 3000
    assert members[0] == ("S00000", 1) and members[-1] == ("S02999", 3000)
    year = coverage_store.con.execute(
        "SELECT candidate_count,eligible_count,selected_count,excluded_listing,excluded_market_cap,excluded_rank,status,available_at FROM item_coverage_cohort_years WHERE fiscal_year=2020"
    ).fetchone()
    assert year[:7] == (3005, 3001, 3000, 3, 1, 1, "complete")
    assert year[7] == dt.datetime(2020, 12, 31, 22)
    # Rebuild is deterministic and the incomplete current year stays explicit.
    _cohort(coverage_store)
    assert coverage_store.con.execute("SELECT count(*) FROM item_coverage_annual_cohort").fetchone()[0] == 3000
    assert coverage_store.con.execute("SELECT status FROM item_coverage_cohort_years ORDER BY fiscal_year").fetchall() == [
        ("complete",),
        ("missing_session",),
        ("incomplete_year",),
    ]


def test_no_historical_listing_evidence_is_not_a_cohort(coverage_store):
    _market_and_listing(coverage_store, n=3)
    coverage_store.con.execute("UPDATE universe_us_listed_membership SET valid_from=DATE '2022-01-01'")
    _cohort(coverage_store)
    assert coverage_store.con.execute("SELECT count(*) FROM item_coverage_annual_cohort").fetchone()[0] == 0
    assert coverage_store.con.execute(
        "SELECT selected_count,excluded_listing,status FROM item_coverage_cohort_years WHERE fiscal_year=2020"
    ).fetchone() == (0, 3, "undersized")


def test_sql_matches_pure_and_includes_absent_registry_item_year(coverage_store):
    _market_and_listing(coverage_store, n=2)
    _market_and_listing(coverage_store, n=1, year=2021)
    _cohort(coverage_store)
    _fact(coverage_store, "S00000", 2020, value=0.0)
    _fact(coverage_store, "S00001", 2021)  # Not a FY2021 cohort member.
    _fact(coverage_store, "S00000", 2020, available=dt.datetime(2023, 1, 1), key="future", value=999)
    sql = measure_item_coverage(coverage_store, OPTIONS)
    facts = coverage_store.con.execute("SELECT * FROM fundamental_standardized").df()
    members = coverage_store.con.execute("SELECT * FROM item_coverage_annual_cohort").df()
    registry = coverage_store.con.execute(
        "SELECT item_id,canonical_code FROM fundamental_item WHERE item_id IN (1001,1101)"
    ).df()
    evidence = coverage_store.con.execute("SELECT * FROM item_coverage_cohort_years").df()
    pure = compute_item_coverage_rows(facts, members, OPTIONS, registry=registry, cohort_years=evidence)
    compare = ["item_id", "basis", "fiscal_year", "n_securities", "n_with_value", "coverage_pct", "cohort_status"]
    pd.testing.assert_frame_equal(sql[compare], pure[compare], check_dtype=False)
    assert sql.iloc[0].n_with_value == 1
    assert sql.loc[sql.fiscal_year == 2021, "n_with_value"].eq(0).all()
    assert sql.loc[sql.item_id == 1101, "n_with_value"].eq(0).all()


def test_historical_revision_is_selected_before_latest_flag(coverage_store):
    _market_and_listing(coverage_store)
    _cohort(coverage_store)
    _fact(coverage_store, "S00000", 2020, latest=False)
    _fact(coverage_store, "S00000", 2020, available=dt.datetime(2023, 1, 1), key="future")
    rows = measure_item_coverage(coverage_store, OPTIONS)
    assert rows.iloc[0].n_with_value == 1


def test_legacy_membership_is_member_and_inclusive_endpoint(coverage_store):
    coverage_store.con.execute(
        """INSERT INTO universe_membership
        (universe_id,security_id,symbol,valid_from,valid_to,as_of_date,available_at,reason,rules_json,
         decision_count,is_latest_revision,source,is_member)
        VALUES ('legacy','A','A',DATE '2019-01-01',DATE '2020-01-01',DATE '2019-01-01',
                TIMESTAMP '2019-01-01','member','{}',1,true,'test',true),
               ('legacy','B','B',DATE '2020-01-01',NULL,DATE '2020-01-01',
                TIMESTAMP '2020-01-01','rejected','{}',1,true,'test',false)"""
    )
    _fact(coverage_store, "A", 2020)
    _fact(coverage_store, "B", 2020)
    rows = measure_item_coverage(coverage_store, replace(OPTIONS, universe_id="legacy"))
    assert (rows.iloc[0].n_securities, rows.iloc[0].n_with_value) == (1, 1)
    assert rows.iloc[0].cohort_status == "non_authoritative_cohort"
    assert evaluate_item_coverage_gate(rows)["items_meeting_threshold"] == 0


def test_empty_refresh_clears_only_requested_slice(coverage_store):
    _market_and_listing(coverage_store)
    _cohort(coverage_store)
    options = replace(OPTIONS, bases=("annual", "quarterly"))
    assert refresh_item_coverage(coverage_store, options) == 12
    empty = pd.DataFrame(columns=ITEM_COVERAGE_COLUMNS)
    assert refresh_item_coverage(coverage_store, replace(OPTIONS, item_ids=(1001,)), frame=empty) == 0
    rows = coverage_store.con.execute(
        "SELECT basis,item_id,count(*) FROM fundamental_item_coverage GROUP BY basis,item_id ORDER BY basis,item_id"
    ).fetchall()
    assert rows == [("annual", 1101, 3), ("quarterly", 1001, 3), ("quarterly", 1101, 3)]


def test_removed_cohort_refresh_cannot_leave_favorable_denominator(coverage_store):
    _market_and_listing(coverage_store)
    _cohort(coverage_store)
    refresh_item_coverage(coverage_store, OPTIONS)
    coverage_store.con.execute("DELETE FROM universe_us_listed_membership")
    _cohort(coverage_store)
    assert coverage_store.con.execute("SELECT count(*) FROM fundamental_item_coverage").fetchone()[0] == 0
    refresh_item_coverage(coverage_store, OPTIONS)
    assert coverage_store.con.execute("SELECT max(n_securities) FROM fundamental_item_coverage").fetchone()[0] == 0


def test_gate_requires_every_completed_year_and_exact_cohort(coverage_store):
    from tests.item_coverage_fixtures import seed_gate_evidence

    seed_gate_evidence(coverage_store, items=tuple(range(1, 111)))
    frame = coverage_store.con.execute("SELECT * FROM fundamental_item_coverage").df()
    gate = evaluate_item_coverage_gate(frame)
    assert gate["status"] == "passed" and gate["items_meeting_threshold"] == 110
    assert gate["completed_fiscal_years"] == list(range(2015, 2024))
    assert coverage_gate_item_count(coverage_store) == 110
    missing = frame[frame.fiscal_year != 2017]
    assert evaluate_item_coverage_gate(missing)["missing_years"] == [2017]
    assert evaluate_item_coverage_gate(missing)["items_meeting_threshold"] == 0
    small = frame.copy()
    small.loc[small.fiscal_year == 2017, "n_securities"] = 2999
    assert evaluate_item_coverage_gate(small)["items_meeting_threshold"] == 0
    assert evaluate_item_coverage_gate(frame, as_of_date=dt.date(2025, 1, 1))["items_meeting_threshold"] == 0
    assert coverage_gate_item_count(coverage_store, as_of_date=dt.date(2025, 1, 1)) == 0


def test_current_year_and_other_bases_cannot_rescue_or_spoil_gate(coverage_store):
    from tests.item_coverage_fixtures import seed_gate_evidence

    seed_gate_evidence(coverage_store, items=(1001,))
    frame = coverage_store.con.execute("SELECT * FROM fundamental_item_coverage").df()
    extra = frame.iloc[[0]].copy()
    extra["fiscal_year"] = 2024
    extra["coverage_pct"] = 0
    extra["n_with_value"] = 0
    quarter = frame.copy()
    quarter["basis"] = "quarterly"
    quarter["coverage_pct"] = 0
    combined = pd.concat([frame, extra, quarter], ignore_index=True)
    assert evaluate_item_coverage_gate(combined, minimum_items=1)["status"] == "passed"


def test_docs_state_actual_cohort_and_incomplete_years():
    rows = compute_item_coverage_rows(pd.DataFrame(), pd.DataFrame(), OPTIONS)
    report = render_item_coverage_markdown(rows, generated_from="test", as_of_date=AS_OF)
    assert DEFAULT_UNIVERSE_ID in report and "current year is incomplete" in report
    assert "status: degraded" in report
    assert report == render_item_coverage_markdown(rows, generated_from="test", as_of_date=AS_OF)


def test_migration_0311_registered_and_catalogued(coverage_store):
    from atx_db.migrations.registry import MIGRATIONS

    versions = [m.version for m in MIGRATIONS]
    assert 311 in versions and versions == sorted(versions)
    assert (
        coverage_store.con.execute(
            "SELECT count(*) FROM table_catalog WHERE table_name IN ('item_coverage_annual_cohort','item_coverage_cohort_years')"
        ).fetchone()[0]
        == 2
    )
    assert (
        coverage_store.con.execute(
            "SELECT count(*) FROM pit_exemption WHERE table_name IN ('item_coverage_annual_cohort','item_coverage_cohort_years')"
        ).fetchone()[0]
        == 2
    )


def test_options_require_explicit_date_and_valid_years():
    with pytest.raises(ValueError, match="fiscal-year"):
        ItemCoverageOptions(as_of_date=AS_OF, minimum_fiscal_year=2023)
    with pytest.raises(ValueError, match="basis"):
        replace(OPTIONS, bases=())


def test_null_cohort_status_or_foreign_source_cannot_pass_gate(coverage_store):
    from tests.item_coverage_fixtures import seed_gate_evidence

    seed_gate_evidence(coverage_store, items=(1001,))
    coverage_store.con.execute("UPDATE fundamental_item_coverage SET cohort_status=NULL WHERE fiscal_year=2018")
    frame = coverage_store.con.execute("SELECT * FROM fundamental_item_coverage").df()
    assert coverage_gate_item_count(coverage_store) == 0
    assert evaluate_item_coverage_gate(frame, minimum_items=1)["status"] == "degraded"
    seed_gate_evidence(coverage_store, items=(1001,))
    coverage_store.con.execute("UPDATE fundamental_item_coverage SET source='other'")
    frame = coverage_store.con.execute("SELECT * FROM fundamental_item_coverage").df()
    assert coverage_gate_item_count(coverage_store) == 0
    assert evaluate_item_coverage_gate(frame, minimum_items=1)["status"] == "degraded"


def test_future_market_revision_does_not_replace_known_ranking_input(coverage_store):
    _market_and_listing(coverage_store, n=2)
    coverage_store.con.execute("UPDATE market_daily_metrics SET is_latest_revision=false")
    coverage_store.con.execute(
        """INSERT INTO market_daily_metrics
        (market_daily_id,source,security_id,trade_date,market_cap,available_at,inputs_hash,as_of_date)
        VALUES ('future','atx-db daily market panel v1','S00001',DATE '2020-12-31',99999,
                TIMESTAMP '2021-01-01','f',DATE '2020-12-31')"""
    )
    _cohort(coverage_store)
    rows = coverage_store.con.execute(
        "SELECT security_id,market_cap,market_daily_id FROM item_coverage_annual_cohort ORDER BY market_cap_rank"
    ).fetchall()
    assert rows == [('S00000', 1000.0, 'm-2020-0'), ('S00001', 1000.0, 'm-2020-1')]
