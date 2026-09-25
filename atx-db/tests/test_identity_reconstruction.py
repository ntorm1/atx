"""RI1: reconstructed historical price-line -> issuer (CIK) links.

Fixtures follow the measured vendor behavior: TickerHistory3 ``shares`` is in
thousands and a new count is backfilled from the issuer's cover ``as of`` date.
Two compact checks (the retained-file measurement is
``scripts/identity_reconstruction_measure.py``): point-in-time availability with
the reconstructed/modeled labels, and preservation of both sides of a two-CIK
conflict.
"""

from __future__ import annotations

import datetime as dt
import json
from dataclasses import replace

import duckdb

from atx_db import historical_identity as hi
from atx_db import identity_reconstruction as ir
from atx_db.market_owner_bridge import PriceLine, classify_strict
from atx_db.migrations.bodies_0327 import create_historical_identity_tables

D = dt.date
HORIZON = D(2018, 12, 31)
PRICE_START = D(2014, 1, 2)
OBSERVED = dt.datetime(2026, 9, 20, 0, 6)


def _con() -> duckdb.DuckDBPyConnection:
    return duckdb.connect(":memory:", config={"memory_limit": "128MB", "threads": 1})


def _quarters(first: dt.date, count: int) -> list[dt.date]:
    return [D(first.year + (first.month - 1 + 3 * i) // 12, (first.month - 1 + 3 * i) % 12 + 1, first.day)
            for i in range(count)]


def _issuer(cik: str, as_ofs: list[dt.date], base: int) -> list[ir.IssuerShareFact]:
    """Quarterly cover counts that change every quarter, filed ten days after ``as_of``."""
    return [
        ir.IssuerShareFact(cik, "dei:EntityCommonStockSharesOutstanding", as_of, float(base + i * 111_111),
                           f"{cik[-4:]}-{as_of:%Y%m%d}", "10-Q", as_of + dt.timedelta(days=10))
        for i, as_of in enumerate(as_ofs)
    ]


def _runs(vendor_id: int, facts: list[ir.IssuerShareFact], last: dt.date) -> list[ir.VendorShareRun]:
    """Vendor runs switching to floor(value / 1000) on each fact's as_of date."""
    starts = sorted((fact.as_of, int(fact.value // 1000)) for fact in facts)
    return [
        ir.VendorShareRun(vendor_id, shares, start,
                          starts[i + 1][0] - dt.timedelta(days=1) if i + 1 < len(starts) else last)
        for i, (start, shares) in enumerate(starts)
    ]


def _line(vendor_id: int, first: dt.date, last: dt.date, symbol: str) -> ir.VendorLine:
    return ir.VendorLine(vendor_id, f"TBLTICKERHISTORY-{vendor_id}", first, last, (last - first).days, symbol,
                         ((symbol, first, last),))


def _run(lines, runs, facts, filings=()) -> ir.ReconstructionResult:
    con = _con()
    ir.stage_share_runs(con, runs)
    ir.stage_share_facts(con, facts)
    return ir.reconstruct_issuer_links(con, lines, filings=filings, horizon=HORIZON, price_start=PRICE_START)


def _at_cutoff(day: dt.date) -> dt.datetime:
    return dt.datetime.combine(day, dt.time(22))


def _load(rows: list[dict[str, object]]) -> duckdb.DuckDBPyConnection:
    """Insert rows into the 0327 DDL (its CHECK constraints apply) and return the connection."""
    con = _con()
    create_historical_identity_tables(con)
    names = [name for name, _ in hi.EVIDENCE_COLUMNS]
    con.executemany(
        f"INSERT INTO security_identity_evidence ({', '.join(names)}) VALUES ({', '.join('?' for _ in names)})",
        [[row[name] for name in names] for row in rows],
    )
    return con


def test_delisted_line_links_point_in_time_as_labelled_reconstructed_evidence():
    cik = "0000000101"
    first, last = D(2015, 1, 2), D(2016, 12, 20)
    facts = _issuer(cik, _quarters(D(2015, 1, 20), 8), 50_123_456)
    runs = _runs(101, facts, last)
    # Form 25 accepted five days before the (Tuesday) last trade: usable only from the next session.
    notice = [ir.IssuerFiling(cik, "25-NSE", D(2016, 12, 15), "25nse", ir._acceptance_utc("2016-12-15T21:05:00.000Z"))]
    line = _line(101, first, last, "OLDCO")
    result = _run([line], runs, facts, notice)

    (link,) = result.links
    assert (link.cik, link.valid_from, link.valid_to, link.tier) == (cik, first, last + dt.timedelta(days=1),
                                                                     ir.TIER_HIGH)
    # Knowable when the third distinct count's 10-Q was public (filed 2015-07-30 + 46h): not at the
    # first bar, and the delisting notice raises the tier, never the clock.
    assert link.available_at == dt.datetime(2015, 7, 31, 22, 0)
    notice_clock = dt.datetime(2016, 12, 21, 22, 0)  # max(acceptance, next session after the last trade)
    assert link.evidence_complete_at == notice_clock
    # The tier is point in time: medium until the Form 25 is usable, high only from its clock.
    assert link.tier_history == ((ir.TIER_MEDIUM, link.available_at), (ir.TIER_HIGH, notice_clock))
    assert link.tier_attained_at == notice_clock
    assert [link.tier_at(_at_cutoff(day)) for day in (D(2015, 7, 30), D(2016, 6, 30), last, D(2016, 12, 21))] == [
        None, ir.TIER_MEDIUM, ir.TIER_MEDIUM, ir.TIER_HIGH]
    # An as-of re-run with only the filings public before that clock cannot link the line.
    assert _run([line], runs, [fact for fact in facts if fact.filed < D(2015, 7, 30)]).links == ()
    # Counts dated outside the line's trading window (same values, five years earlier) never match.
    early = [replace(fact, as_of=fact.as_of - dt.timedelta(days=1826), filed=fact.filed - dt.timedelta(days=1826))
             for fact in facts]
    outside = _run([line], runs, early)
    assert (outside.links, outside.rejected) == ((), ())

    rows = result.evidence_rows(observed_at=OBSERVED, artifact_sha256="cd" * 32, run_id="ri1-test")
    (row,) = rows
    payload = json.loads(row["value_json"])
    assert (row["fact_kind"], row["evidence_status"], row["availability_status"], row["method"]) == (
        "issuer_link", "reconstructed", "modeled", ir.METHOD)
    assert (payload["identity_basis"], payload["tier"], payload["corroborated_by"]) == (
        ir.IDENTITY_BASIS, ir.TIER_HIGH, [ir.EV_TERMINAL])
    assert (payload["tier_at_available_at"], payload["tier_attained_at"], payload["tier_history"]) == (
        ir.TIER_MEDIUM, notice_clock.isoformat(),
        [[ir.TIER_MEDIUM, link.available_at.isoformat()], [ir.TIER_HIGH, notice_clock.isoformat()]])
    assert row["available_at"] == link.available_at and payload["vendor_shares_unit"] == "thousands"
    assert set(hi.audit_identity_evidence(_load(rows)).values()) == {0}
    # Strict (certification) mode refuses the reconstructed evidence.
    _bridge, _members, _ambiguous, rejected = classify_strict(
        [PriceLine("TBLTICKERHISTORY-101", "OLDCO", first, last, 500)], result.owner_links(), {})
    assert rejected == {"evidence_not_verified_dated": 1}


def test_two_cik_conflicts_keep_both_sides_and_resolve_only_by_dominance():
    first, last = D(2015, 1, 2), D(2017, 6, 30)
    as_ofs = _quarters(D(2015, 1, 20), 10)
    # (a) Co-registrants report identical counts: unresolved -- no link, both CIKs kept as conflicting.
    parent, twin = "0000000401", "0000000402"
    coreg = _issuer(parent, as_ofs, 30_000_001) + _issuer(twin, as_ofs, 30_000_001)
    # (b) A subsidiary co-reports the parent's first two counts: the parent dominates (>= 3x weight).
    owner, sub = "0000000411", "0000000412"
    owner_facts = _issuer(owner, as_ofs, 60_000_001)
    sub_facts = [replace(fact, cik=sub, accession=f"s-{fact.as_of}") for fact in owner_facts[:2]]
    result = _run(
        [_line(401, first, last, "COREG"), _line(411, first, last, "DOM")],
        _runs(401, coreg[:10], last) + _runs(411, owner_facts, last),
        coreg + owner_facts + sub_facts,
    )

    (link,) = result.links
    assert (link.vendor_id, link.cik, link.tier, [cik for cik, _w in link.competitors]) == (
        411, owner, ir.TIER_LOW, [sub])
    rows = result.evidence_rows(observed_at=OBSERVED)
    kept = sorted(
        (row["native_key"], row["cik"], row["evidence_status"], row["rejection_reason"], row["available_at"],
         [cik for cik, _w in json.loads(row["value_json"])["competitors"]])
        for row in rows
    )
    assert kept == [
        ("401", parent, "conflicting", ir.REJECT_CONFLICTING, None, [twin]),
        ("401", twin, "conflicting", ir.REJECT_CONFLICTING, None, [parent]),
        ("411", owner, "reconstructed", None, link.available_at, [sub]),
        ("411", sub, "conflicting", ir.REJECT_CONFLICT_DOMINATED, None, [owner]),
    ]
    assert result.summary()["unresolved_conflict_lines"] == 1
    # Rejected sides are never linkable, so the accepted-overlap audit stays clean.
    assert set(hi.audit_identity_evidence(_load(rows)).values()) == {0}
