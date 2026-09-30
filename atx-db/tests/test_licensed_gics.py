"""GICS adapter (S8.1): publication clock, effective dates, structure guard against restated histories."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.licensed import gics as G
from atx_db.licensed.mock import MockUniverse

BUILD = dt.datetime(2026, 7, 2)


@pytest.fixture(scope="module")
def run(tmp_path_factory: pytest.TempPathFactory):
    u = MockUniverse()
    raw = tmp_path_factory.mktemp("gics")
    G.ADAPTER.mock(raw, u)
    st = G.ADAPTER.load(raw, identity=u.resolver(), build_time=BUILD)
    return u, st, st.tables["gics_history"].to_pylist()


def test_schema_levels_and_validation(run) -> None:
    _, st, rows = run
    assert st.tables["gics_history"].schema == G.GICS.schema and not st.report.failures
    for r in rows:
        s = r["gics_sub_industry"]
        assert len(s) == 8 and (r["gics_sector"], r["gics_group"], r["gics_industry"]) == (s[:2], s[:4], s[:6])


def test_announced_reclassification_clock(run) -> None:
    u, _, rows = run
    sid = u.lines[0].security_id
    spells = sorted((r for r in rows if r["security_id"] == sid), key=lambda r: r["effective_from"])
    assert [r["gics_sub_industry"] for r in spells] == ["45102020", "40201060"]
    new = spells[1]
    assert new["effective_from"] == dt.date(2023, 3, 20) and new["clock_basis"] == "vendor_pit"
    assert new["available_at"] == dt.datetime(2023, 3, 7, 4, 59, 59)  # published 2023-03-06, before effect
    assert spells[0]["effective_to"] == dt.date(2023, 3, 19) and all(r["structure_ok"] for r in spells)


def test_structure_guard_clips_restated_history(run) -> None:
    u, _, rows = run
    r = next(r for r in rows if r["security_id"] == u.lines[8].security_id)
    assert r["effective_from_vendor"] == dt.date(2015, 1, 2) and r["effective_from"] == dt.date(2018, 10, 1)
    assert r["clock_basis"] == "structure_guard" and r["vintage_risk"]
    assert r["available_at"] == dt.datetime(2018, 10, 1, 4, 0) and r["link_tier"] == "cusip_undated"


def test_floor_without_publication_date(run) -> None:
    _, _, rows = run
    fl = [r for r in rows if r["clock_basis"] == "floor"]
    assert fl and all(r["vintage_risk"] and r["available_at"].date() == r["effective_from"] for r in fl)


def test_discontinued_code_after_its_end_fails_structure(tmp_path) -> None:
    u = MockUniverse()
    from atx_db.licensed import contract as K
    ln = u.lines[1]
    p = K.csv_write(tmp_path / "gics_bad.csv", ["CUSIP", "TICKER", "GSUBIND", "INDFROM", "INDTHRU", "SNAPSHOTDATE"],
                    [[ln.cusip_on(dt.date(2024, 1, 2)), None, "45102020", "2024-01-02", None, "2023-12-20"]])
    K.write_receipt(tmp_path, p, fetched_at=dt.datetime(2026, 7, 1), history_mode="pit_archive")
    st = G.ADAPTER.load(tmp_path, identity=u.resolver(), build_time=BUILD)
    assert st.tables["gics_history"].column("structure_ok").to_pylist() == [False]
    assert st.report.failed("gics_history", "structure_ok") and not st.report.failures  # reported, not fatal


def test_substitute_is_classification() -> None:
    sub = G.ADAPTER.substitute()
    assert sub.stage == "classification/issuer_industry.parquet" and sub.owner.startswith("MKT")
    assert sub.keys == ("cik", "available_at") and sub.columns["gics_sub_industry"] == "naics2022"
