"""Transcripts adapter (S8.1): version vintages, creation-time clock, delivery clock without one, components."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.licensed import transcripts as T
from atx_db.licensed.mock import MockUniverse

BUILD = dt.datetime(2026, 7, 2)


@pytest.fixture(scope="module")
def run(tmp_path_factory: pytest.TempPathFactory):
    u = MockUniverse()
    raw = tmp_path_factory.mktemp("tr")
    T.ADAPTER.mock(raw, u)
    st = T.ADAPTER.load(raw, identity=u.resolver(), build_time=BUILD)
    return u, st, st.tables["calls"].to_pylist(), st.tables["components"].to_pylist()


def test_schema_and_validation(run) -> None:
    _, st, calls, comps = run
    assert st.tables["calls"].schema == T.CALLS.schema and st.tables["components"].schema == T.COMPONENTS.schema
    assert not st.report.failures and st.rejects["components"] == {"orphan_component": 1}
    assert len(comps) == 4 * len(calls)


def test_versions_are_vintages_clocked_at_creation(run) -> None:
    u, _, calls, _ = run
    ev = [r for r in calls if r["event_id"] == f"{u.lines[0].security_id}0"]
    ev.sort(key=lambda r: r["available_at"])
    assert [r["version"] for r in ev] == ["Preliminary", "Edited"]
    assert ev[0]["event_at"] == dt.datetime(2024, 1, 25, 21, 0)
    assert ev[0]["available_at"] == dt.datetime(2024, 1, 26, 0, 0) and ev[1]["available_at"] == dt.datetime(2024, 1, 27, 3, 0)
    assert all(r["available_at"] >= r["event_at"] for r in calls)


def test_version_without_creation_time_waits_for_delivery(run) -> None:
    u, _, calls, comps = run
    aud = [r for r in calls if r["version"] == "Audited Copy"]
    assert len(aud) == 1 and aud[0]["clock_basis"] == "delivery" and aud[0]["vintage_risk"]
    assert aud[0]["available_at"] == dt.datetime(2026, 7, 1, 6, 0) and aud[0]["security_id"] == u.lines[1].security_id
    c = [r for r in comps if r["transcript_id"] == aud[0]["transcript_id"]]
    assert len(c) == 4 and {r["available_at"] for r in c} == {aud[0]["available_at"]}


def test_components_inherit_identity_and_text_hash(run) -> None:
    _, _, calls, comps = run
    call = calls[0]
    c = sorted((r for r in comps if r["transcript_id"] == call["transcript_id"]), key=lambda r: r["component_order"])
    assert {(r["security_id"], r["cik"], r["available_at"]) for r in c} == {(call["security_id"], call["cik"], call["available_at"])}
    import hashlib
    assert call["text_sha256"] == hashlib.sha256("\n".join(r["text"] for r in c).encode()).hexdigest()
    assert call["n_words"] == sum(r["n_words"] for r in c) and call["n_components"] == 4
    assert {r["speaker_type"] for r in c} == {"Operator", "Executives", "Analysts"}


def test_no_free_substitute() -> None:
    assert T.ADAPTER.substitute() is None
