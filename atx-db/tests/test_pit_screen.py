"""End-to-end point-in-time contract of the universe cross-section screen (task P7).

One warehouse (the R2a panel fixture plus a later-delisted member, an explicit
NULL restatement, a withheld market revision and standardized items) screened at
several knowledge cutoffs: nothing known only after the cutoff appears, a member
delisted after the cutoff is listed with its as-of state, a newer NULL state is
never replaced by an older value, and a month-end screen equals the R2a panel
formation byte for byte.
"""

from __future__ import annotations

import datetime as dt

from atx_db import fundamental_signal_research as fsr
from atx_db.asof import cross_section_asof
from atx_db.market_daily import MARKET_DAILY_SOURCE_NAME
from atx_db.research import panel as rp
from atx_db.research.store import ResearchStore
from tests.test_research_panel import FUND, LINES, Warehouse, _at, _definitions, _options, _populate

DDD = "SEC-CIK-0000000004"
FIELDS = ("roa_q", "accruals_ttm", "market_cap", "momentum_12_1", "item:revenue:quarterly")
REVENUE = "item:revenue:quarterly"


def _utc(*parts: int) -> dt.datetime:
    return dt.datetime(*parts, tzinfo=dt.UTC)


def _rows(result) -> dict[tuple[str, str], object]:
    return {(row.field_id, row.security_id): row for row in result.rows.itertuples(index=False)}


def _revenue(wh: Warehouse, state_id: str, cik: int, period_end: dt.date, value: float | None,
             clock: dt.datetime) -> None:
    wh.con.execute("""
        INSERT INTO fundamental_standardized BY NAME
        SELECT ? AS standardized_id, ? AS security_id, 'revenue' AS canonical_code, ? AS cik,
               'quarterly' AS basis, 'companyfacts' AS source, CAST(? AS DATE) AS period_start,
               CAST(? AS DATE) AS period_end, CAST(? AS TIMESTAMP) AS available_at, CAST(? AS DOUBLE) AS value,
               CAST(? AS DATE) AS as_of_date, 'rev_q' AS rule_id
    """, [state_id, f"SEC-COMPANYFACTS-UNRESOLVED-CIK-{cik:010d}", f"{cik:010d}",
          period_end - dt.timedelta(days=91), period_end, clock, value, clock.date()])


def _build(tmp_path, monkeypatch):
    defs = _definitions()
    monkeypatch.setattr(fsr, "default_derived_definitions", lambda: defs)
    path = tmp_path / "wh.duckdb"
    wh = Warehouse(path)
    ids = _populate(wh)
    # A listed common member that is delisted after the November screen.
    wh.listing(4, DDD, "DDD", "common", "XNAS", first=dt.date(2023, 9, 1), last=dt.date(2023, 12, 15))
    # CCC's Q3 roa is restated to an explicit NULL state before the November cutoff.
    ids["q3_3_null"] = wh.state(3, "roa_q", "q", dt.date(2023, 9, 30), None, _at("2023-11-20"))
    # A later market revision for BBB on 2023-11-30 withholds market_cap (NULL).
    bbb = LINES[2][0]
    wh.con.execute("""
        INSERT INTO market_daily_metrics
        VALUES (?,?,?,DATE '2023-11-30',TIMESTAMP '2023-11-30 21:30:00',DATE '2023-11-30',100.0,1000,NULL,NULL,
                0.1,'split_unresolved')
    """, [f"{bbb}|2023-11-30|r2", MARKET_DAILY_SOURCE_NAME, bbb])
    # Standardized items (the fixture table gains the real schema's PIT columns).
    for ddl in ("as_of_date DATE", "valid_to TIMESTAMP", "rule_id VARCHAR"):
        wh.con.execute(f"ALTER TABLE fundamental_standardized ADD COLUMN {ddl}")
    _revenue(wh, "rev_1_q3", 1, dt.date(2023, 9, 30), 5.0e9, _at("2023-11-09"))
    # reported_eps_conflict_v1 shape: a later, visible explicit NULL state.
    _revenue(wh, "rev_1_q3_null", 1, dt.date(2023, 9, 30), None, _at("2023-12-05"))
    # Filed 30 minutes after the January month-end cutoff.
    _revenue(wh, "rev_1_q4_late", 1, dt.date(2023, 12, 31), 7.0e9, _at("2024-01-31", 22, 30))
    _revenue(wh, "rev_2_q3", 2, dt.date(2023, 9, 30), 3.0e9, _at("2023-11-09"))
    wh.close()
    return path, ids


def test_pit_screen_end_to_end(tmp_path, monkeypatch):
    path, ids = _build(tmp_path, monkeypatch)
    aaa, bbb, ccc = (LINES[i][0] for i in (1, 2, 3))
    january = dt.date(2024, 1, 31)
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=path) as store:
        assert rp.build_research_panel(store, _options("recon")).status == "complete"
        panel_digests = dict(store.con.execute("""
            SELECT feature_id, values_sha256 FROM research_panel_coverage
            WHERE run_id='recon' AND formation_date=?
        """, [january]).fetchall())

    # Month-end cutoff: exactly the R2a formation (same rows, same digests).
    jan = cross_section_asof(_utc(2024, 1, 31, 22), FIELDS, path, scratch_dir=tmp_path)
    assert (jan.status, jan.screen_date, jan.basis, jan.universe_id) == (
        "complete", january, "reconstructed", "us_listed_reconstructed_v1")
    assert jan.r2a_field_digests == panel_digests and len(panel_digests) == 4
    rows = _rows(jan)

    # A fact filed after the cutoff never appears: AAA's Q4 roa and Q4 revenue
    # (22:30) are invisible at 22:00; nothing in the screen is clocked later.
    assert rows[("roa_q", aaa)].derived_value_id == ids["q3_1_rev"] and rows[("roa_q", aaa)].value == 0.025
    assert (jan.rows["available_at"].dropna() <= dt.datetime(2024, 1, 31, 22)).all()
    assert (jan.rows["latest_input_clock"].dropna() <= dt.datetime(2024, 1, 31, 22)).all()
    # Newer NULL suppresses older value: AAA's Q3 revenue conflict state wins
    # over the earlier 5.0e9; BBB's plain Q3 value is valid.
    revenue = rows[(REVENUE, aaa)]
    assert (revenue.value_status, revenue.source_ref) == ("null_state", "rev_1_q3_null")
    assert revenue.value != revenue.value  # NaN: no value
    assert (rows[(REVENUE, bbb)].value, rows[(REVENUE, bbb)].value_status) == (3.0e9, "valid")
    assert rows[(REVENUE, bbb)].staleness_days == (january - dt.date(2023, 9, 30)).days
    # Bases are labeled on every row; the not-common fund keeps NULL rows.
    assert set(jan.rows.loc[jan.rows.eligible.astype(bool) & (jan.rows.field_id == "roa_q"), "identity_basis"]) \
        == {"current_ticker_unverified"}
    assert set(jan.rows["universe_basis"]) == {"us_listed_reconstructed_v1"}
    assert rows[("roa_q", FUND)].value_status == "not_common"
    assert rows[("market_cap", aaa)].size_status == rp.SIZE_VERIFIED
    assert rows[("market_cap", bbb)].size_status == rp.UNVERIFIED_VENDOR_SHARES
    assert "reconstructed_identity_universe_and_availability_not_certifiable" in jan.blockers
    assert any(b.startswith("unverified_vendor_share_size_rows:") for b in jan.blockers)
    assert DDD not in set(jan.rows.security_id)  # delisted on 2023-12-15

    # Thirty minutes later the late filings are visible.
    late = _rows(cross_section_asof(_utc(2024, 1, 31, 22, 30), FIELDS, path, scratch_dir=tmp_path))
    assert late[("roa_q", aaa)].derived_value_id == ids["q4_1_late"]
    assert (late[(REVENUE, aaa)].value, late[(REVENUE, aaa)].period_end) == (7.0e9, dt.datetime(2023, 12, 31))
    # Intraday, the day's market rows (clocked 21:00) are not yet known: the
    # screen session is the previous one.
    assert cross_section_asof(_utc(2024, 1, 31, 15), ("momentum_12_1",), path,
                              scratch_dir=tmp_path).screen_date == dt.date(2024, 1, 30)

    # A member delisted after the cutoff is still listed, with its as-of state.
    nov = cross_section_asof(_utc(2023, 11, 30, 22), FIELDS, path, scratch_dir=tmp_path)
    rows = _rows(nov)
    member = rows[("momentum_12_1", DDD)]
    assert (member.symbol, member.exchange_code, member.membership_reason, member.cohort_reason) == (
        "DDD", "XNAS", "member", "valid")
    assert (member.value, member.value_status) == (0.1, "valid")
    assert "valid_to" not in nov.rows.columns
    # Newer NULL states win for derived and market fields too.
    restated = rows[("roa_q", ccc)]
    assert restated.derived_value_id == ids["q3_3_null"] and restated.value_status != "valid"
    assert restated.value != restated.value  # NaN: the earlier 0.05 is not revived
    withheld = rows[("market_cap", bbb)]
    assert withheld.value_status == "invalid_current_state" and withheld.value != withheld.value
    assert nov.diagnostics["market_null_revision_guard_rows"] == 1
    assert rows[("market_cap", aaa)].value_status == "valid"
