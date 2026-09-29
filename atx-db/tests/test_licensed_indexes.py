"""Index constituents and weights adapter (S8.1): announcement clocks, survivorship, weight normalization."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.licensed import indexes as X
from atx_db.licensed.mock import UNKNOWN_CUSIP, MockUniverse

BUILD = dt.datetime(2026, 7, 2)


@pytest.fixture(scope="module")
def run(tmp_path_factory: pytest.TempPathFactory):
    u = MockUniverse()
    raw = tmp_path_factory.mktemp("idx")
    X.ADAPTER.mock(raw, u)
    st = X.ADAPTER.load(raw, identity=u.resolver(), build_time=BUILD)
    return u, st, st.tables["constituents"].to_pylist(), st.tables["weights"].to_pylist()


def test_schema_and_validation(run) -> None:
    _, st, cons, w = run
    assert st.tables["constituents"].schema == X.CONSTITUENTS.schema and st.tables["weights"].schema == X.WEIGHTS.schema
    assert cons and w and not st.report.failures


def test_announced_addition_known_before_effective(run) -> None:
    u, _, cons, _ = run
    r = next(r for r in cons if r["security_id"] == u.lines[9].security_id)
    assert r["effective_from"] == dt.date(2024, 6, 24) and r["announced_before_effective"]
    assert r["available_at"] == dt.datetime(2024, 6, 7, 21, 15) and r["clock_basis"] == "vendor_pit"


def test_spell_without_announcement_is_floor_at_open(run) -> None:
    _, _, cons, _ = run
    fl = [r for r in cons if r["clock_basis"] == "floor"]
    assert fl and all(r["available_at"] == dt.datetime.combine(r["effective_from"], dt.time(14, 30)) for r in fl)


def test_delisted_member_kept_with_end_date(run) -> None:
    u, _, cons, _ = run
    r = next(r for r in cons if r["security_id"] == u.lines[2].security_id)
    assert r["effective_to"] == dt.date(2022, 7, 1)
    unk = [r for r in cons if r["cusip"] == UNKNOWN_CUSIP]
    assert unk and unk[0]["link_tier"] == "unmapped"


def test_weights_normalized_and_evening_clock(run) -> None:
    _, _, _, w = run
    by: dict = {}
    for r in w:
        by.setdefault(r["as_of_date"], 0.0)
        by[r["as_of_date"]] += r["weight"]
    assert all(abs(s - 1) < 1e-4 for s in by.values())
    r = w[0]
    assert r["available_at"] == dt.datetime.combine(r["as_of_date"] + dt.timedelta(days=1), dt.time(4, 59, 59))


def test_substitute_is_index_proxies() -> None:
    sub = X.ADAPTER.substitute()
    assert sub.stage == "indexes" and sub.owner.startswith("MKT")
