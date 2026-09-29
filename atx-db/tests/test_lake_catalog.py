"""catalog.duckdb on a fixture lake: views, hive partitions, as_of macros (event and vintage), comments,
byte-identical rebuilds and read-only opens."""

from __future__ import annotations

import datetime as dt
import hashlib
from pathlib import Path

import duckdb

from atx_db.lake.catalog import build, canonical_dump
from atx_db.lake.contract import Output, Stage, validate
from atx_db.lake.testing import publish, write_table

D = dt.datetime


def _lake(tmp_path: Path) -> tuple[Path, list[Stage]]:
    root = tmp_path / "lake"
    write_table(root, "calendar.parquet", {"session_date": [dt.date(2024, 1, d) for d in (2, 3, 4, 5)]})
    write_table(root, "px/year=2023/px.parquet", {"session_date": [dt.date(2023, 12, 29)], "security_id": [1],
                                                  "close": [9.0]})
    write_table(root, "px/year=2024/px.parquet", {"session_date": [dt.date(2024, 1, 2), dt.date(2024, 1, 3)],
                                                  "security_id": [1, 1], "close": [10.0, 11.0]})
    publish(root, "px/manifest.json")
    # fundamentals-like vintage table: cik 7 restated on 2024-01-04; tz-aware clock on the event table
    write_table(root, "fund/events.parquet", {
        "cik": [7, 7, 8], "accession": ["a1", "a2", "b1"], "at": [100.0, 120.0, 5.0],
        "clock_utc": [D(2024, 1, 2, 21), D(2024, 1, 4, 13), D(2024, 1, 3, 12)]})
    write_table(root, "fund/news.parquet", {"cik": [7, 8], "available_at": [
        D(2024, 1, 2, 21, tzinfo=dt.UTC), D(2024, 1, 4, 21, tzinfo=dt.UTC)]})
    write_table(root, "fund/lookup.parquet", {"cik": [7, 8], "name": ["x", "y"]})
    publish(root, "fund/manifest.json")
    stages = validate([
        Stage("px", "MKT", "atx.test.px/v1", None, doc="fixture", vintage="daily",
              outputs=(Output("px/year=*/px.parquet", view="px", clock="session_date",
                              columns={"close": "fixture close"}),
                       Output("calendar.parquet", view="calendar", clock=None))),
        Stage("fund", "FUND", "atx.test.fund/v1", None, doc="fixture", vintage="vintage",
              outputs=(Output("fund/events.parquet", view="fund_events", clock="clock_utc", keys=("cik",),
                              order=("accession",)),
                       Output("fund/news.parquet", view="fund_news", vintage="event"),
                       Output("fund/lookup.parquet", view="fund_lookup", clock=None),
                       Output("fund/absent_*.parquet", view="fund_absent", vintage="event"))),
        Stage("later", "EVT", None, None, doc="fixture", planned=True, outputs=(Output("later/x.parquet"),)),
    ])
    return root, stages


def test_views_macros_and_comments(tmp_path: Path) -> None:
    root, stages = _lake(tmp_path)
    dest = tmp_path / "out" / "catalog.duckdb"
    rec = build(root, stages, dest, column_docs={"security_id": "line key"})
    assert sorted(rec["views"]) == ["calendar", "fund_events", "fund_lookup", "fund_news", "px"]
    assert sorted(rec["macros"]) == ["fund_events_as_of", "fund_news_as_of", "lake_cutoff", "px_as_of"]
    assert [s["view"] for s in rec["skipped"]] == ["fund_absent", "later"]  # no file yet: no view
    con = duckdb.connect(str(dest), read_only=True)
    try:
        # hive partition column from year=YYYY
        assert con.execute("SELECT count(*), min(year), max(year) FROM px").fetchone() == (3, 2023, 2024)
        # DATE clock = 22:00 UTC of the session: the 2024-01-03 bar is visible after 2024-01-03 22:00
        assert con.execute("SELECT count(*) FROM px_as_of(TIMESTAMP '2024-01-03 22:00')").fetchone()[0] == 2
        assert con.execute("SELECT count(*) FROM px_as_of(TIMESTAMP '2024-01-03 22:00:01')").fetchone()[0] == 3
        # vintage: latest version per cik strictly before ts
        q = "SELECT cik, accession, \"at\" FROM fund_events_as_of(?) ORDER BY cik"
        assert con.execute(q, [D(2024, 1, 4, 13)]).fetchall() == [(7, "a1", 100.0), (8, "b1", 5.0)]
        assert con.execute(q, [D(2024, 1, 4, 14)]).fetchall() == [(7, "a2", 120.0), (8, "b1", 5.0)]
        # TIMESTAMPTZ clock compared in UTC whatever the reader's TimeZone
        con.execute("SET TimeZone='America/New_York'")
        assert con.execute("SELECT cik FROM fund_news_as_of(TIMESTAMP '2024-01-03 00:00')").fetchall() == [(7,)]
        # decision cutoff of session 2024-01-04 = 22:00 UTC of 2024-01-03
        assert con.execute("SELECT lake_cutoff(DATE '2024-01-04')").fetchone()[0] == D(2024, 1, 3, 22)
        comments = dict(con.execute("SELECT view_name, comment FROM duckdb_views() WHERE NOT internal").fetchall())
        assert "vintage vintage" in comments["fund_events"] and "manifest fund/manifest.json sha256" in comments["fund_events"]
        cols = dict(con.execute("SELECT column_name, comment FROM duckdb_columns() WHERE table_name = 'px' AND "
                                "comment IS NOT NULL").fetchall())
        assert cols == {"close": "fixture close", "security_id": "line key"}
        rows = con.execute("SELECT name, manifest_sha256 IS NOT NULL, planned FROM lake_stages ORDER BY name").fetchall()
        assert rows == [("fund", True, False), ("later", False, True), ("px", True, False)]
    finally:
        con.close()


def test_two_rebuilds_are_byte_identical_and_dump_stable(tmp_path: Path) -> None:
    root, stages = _lake(tmp_path)
    dest = tmp_path / "out" / "catalog.duckdb"
    first = build(root, stages, dest)
    dump1 = canonical_dump(dest)
    second = build(root, stages, dest)
    assert first["sha256"] == second["sha256"] == hashlib.sha256(dest.read_bytes()).hexdigest()
    assert canonical_dump(dest) == dump1 and "MACRO fund_events_as_of(ts)" in dump1
    assert not (dest.parent / "_catalog_build").exists()


def test_read_only_open_while_no_writer(tmp_path: Path) -> None:
    root, stages = _lake(tmp_path)
    dest = tmp_path / "out" / "catalog.duckdb"
    build(root, stages, dest)
    a = duckdb.connect(str(dest), read_only=True)
    b = duckdb.connect(str(dest), read_only=True)
    try:
        assert a.execute("SELECT count(*) FROM fund_lookup").fetchone()[0] == 2
        assert b.execute("SELECT count(*) FROM calendar").fetchone()[0] == 4
    finally:
        a.close()
        b.close()
