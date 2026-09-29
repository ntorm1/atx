"""Securities-lending adapter (S8.1): daily files, publication lag, backfill history, domains, substitute."""

from __future__ import annotations

import datetime as dt
from pathlib import Path

import pytest

from atx_db.licensed import contract as K
from atx_db.licensed import lending as L
from atx_db.licensed.mock import UNKNOWN_CUSIP, MockUniverse

BUILD = dt.datetime(2026, 7, 2)


@pytest.fixture(scope="module")
def run(tmp_path_factory: pytest.TempPathFactory):
    u = MockUniverse()
    raw = tmp_path_factory.mktemp("lend")
    L.ADAPTER.mock(raw, u)
    st = L.ADAPTER.load(raw, identity=u.resolver(), build_time=BUILD)
    return u, raw, st, st.tables["lending_daily"].to_pylist()


def test_schema_and_validation(run) -> None:
    _, _, st, rows = run
    assert st.tables["lending_daily"].schema == L.LENDING.schema and len(rows) > 900
    assert not st.report.failures


def test_publication_lag_next_weekday_noon_utc(run) -> None:
    _, _, _, rows = run
    fri = [r for r in rows if r["data_date"] == dt.date(2024, 1, 12)]  # a Friday file
    assert fri and all(r["available_at"] == dt.datetime(2024, 1, 15, 12, 0) for r in fri)
    assert all(r["vendor_snapshot_at"] == dt.datetime(2024, 1, 13, 4, 59, 59) for r in fri)
    # daily pull delivered at 07:05 UTC, before the rule clock: the rule wins
    assert {r["clock_basis"] for r in fri} == {"publication_lag"} and {r["history_mode"] for r in fri} == {"daily"}


def test_daily_pull_later_than_rule_uses_delivery(run, tmp_path: Path) -> None:
    u, raw, _, _ = run
    src = raw / "lending_20240112.csv"
    p = tmp_path / src.name
    p.write_bytes(src.read_bytes())
    K.write_receipt(tmp_path, p, fetched_at=dt.datetime(2024, 1, 16, 15, 0), history_mode="daily")
    rows = L.ADAPTER.load(tmp_path, identity=u.resolver(), build_time=BUILD).tables["lending_daily"].to_pylist()
    assert {(r["available_at"], r["clock_basis"]) for r in rows} == {(dt.datetime(2024, 1, 16, 15, 0), "delivery")}


def test_history_recut_is_backfill(run) -> None:
    u, _, _, rows = run
    h = [r for r in rows if r["source_file"] == "lending_history_recut.csv"]
    assert len(h) == 5 and all(r["available_at"] == dt.datetime(2026, 6, 30, 9, 0) and r["vintage_risk"] for r in h)
    assert {r["security_id"] for r in h} == {u.lines[2].security_id}  # the delisted line, mapped by its 2021 CUSIP


def test_identifiers_and_domains(run, tmp_path: Path) -> None:
    _, _, st, rows = run
    unk = [r for r in rows if r["cusip"] == UNKNOWN_CUSIP]
    assert unk and {r["link_tier"] for r in unk} == {"unmapped"}
    assert all(r["isin"][2:11] == r["cusip"] for r in rows if r["isin"])
    assert st.report.stats["lending_daily"]["link_tiers"]["cusip_dated"] == len(rows) - len(unk)
    bad = st.tables["lending_daily"]
    vals = bad.column("utilization_pct").to_pylist()
    vals[0] = 120.0
    import pyarrow as pa
    bad = bad.set_column(bad.schema.get_field_index("utilization_pct"), "utilization_pct", pa.array(vals))
    assert L.ADAPTER.validate({"lending_daily": bad}, BUILD).failed("lending_daily", "utilization_domain")


def test_substitute_is_borrow_proxy() -> None:
    sub = L.ADAPTER.substitute()
    assert sub.stage == "borrow_proxy" and sub.columns["utilization_pct"] == "si_to_io"
    assert sub.columns["lendable_quantity"] == "inst_shares" and "security_id" in sub.keys
