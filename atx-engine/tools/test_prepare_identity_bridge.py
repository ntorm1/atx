"""Synthetic postimplementation checks for prepare_identity_bridge; no real r4 export, role or warehouse."""
import datetime as dt
import hashlib
import io
import json
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

import prepare_identity_bridge as tool

D = dt.date
T = dt.datetime
LINK_SCHEMA = pa.schema([
    ("perm_security_id", pa.int64()), ("perm_company_id", pa.int64()), ("cik", pa.string()),
    ("link_start", pa.date32()), ("link_end", pa.date32()), ("link_basis", pa.string()),
    ("link_primary", pa.string()), ("tier", pa.string()), ("available_at", pa.timestamp("us")),
    ("evidence_ids", pa.string()), ("created_at", pa.timestamp("us")), ("valid_until", pa.timestamp("us")),
    ("class_status", pa.string())])
PERM_SCHEMA = pa.schema([("perm_security_id", pa.int64()), ("security_id", pa.string()),
                         ("first_trade_date", pa.date32()), ("last_trade_date", pa.date32()),
                         ("created_at", pa.timestamp("us"))])


def link(sr, cik, start, end, avail, primary="P", tier="high", basis="reconstructed_high", until=None,
         status="common"):
    return {"perm_security_id": sr, "perm_company_id": cik, "cik": f"{cik:010d}", "link_start": start,
            "link_end": end, "link_basis": basis, "link_primary": primary, "tier": tier, "available_at": avail,
            "evidence_ids": "[]", "created_at": T(2026, 9, 27), "valid_until": until, "class_status": status}


def parquet_bytes(rows, schema):
    sink = io.BytesIO()
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), sink)
    return sink.getvalue()


def make_source(root: Path, links, extra_lines=()):
    src = root / "r4"
    (src / "phases/export-001").mkdir(parents=True)
    lines = sorted({r["perm_security_id"] for r in links} | set(extra_lines))
    perm = [{"perm_security_id": i, "security_id": f"TBLTICKERHISTORY-{i}", "first_trade_date": D(2012, 1, 3),
             "last_trade_date": D(2026, 9, 18), "created_at": T(2026, 9, 27)} for i in lines]
    files = []
    for dataset, rows, schema in (("security_permanent_ids", perm, PERM_SCHEMA),
                                  ("security_company_links", links, LINK_SCHEMA)):
        blob = parquet_bytes(rows, schema)
        rel = f"phases/export-001/{dataset}.part-0000.parquet"
        (src / rel).write_bytes(blob)
        files.append({"dataset": dataset, "table": dataset, "file": rel, "bytes": len(blob), "rows": len(rows),
                      "sha256": hashlib.sha256(blob).hexdigest(), "schema": []})
    manifest = {"schema_version": "identity_links_v1", "rehearsal": True, "scope_complete": False,
                "snapshot_date": "2026-09-20", "files": files, "remaining_acceptance": ["repeatability"],
                "end_date_semantics": "inclusive business end; valid_until is exclusive evidence-version end"}
    (src / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return src


def rows_of(out: Path):
    return pq.read_table(out / "links.parquet").to_pylist()


def by_line(rows, sr):
    return [(r["cik"], r["start"], r["end_incl"], r["primary"], r["tier"]) for r in rows if r["sr_id"] == sr]


# ---------------------------------------------------------------------------
# Rule semantics
# ---------------------------------------------------------------------------

def test_mark_boundary_is_inclusive_at_22_utc():
    rows, _ = tool.resolve([link(101, 1, D(2020, 1, 2), D(2020, 1, 10), T(2020, 1, 3, 22)),
                            link(102, 2, D(2020, 1, 2), D(2020, 1, 10), T(2020, 1, 3, 22, 0, 1))])
    assert by_line(rows, 101) == [(1, D(2020, 1, 3), D(2020, 1, 10), "P", "high")]
    assert by_line(rows, 102) == [(2, D(2020, 1, 4), D(2020, 1, 10), "P", "high")]
    assert rows[0]["available_at"] == T(2020, 1, 3, 22)
    # business start after the clock: the business start governs
    rows, _ = tool.resolve([link(103, 3, D(2020, 2, 1), D(2020, 2, 5), T(2020, 1, 1))])
    assert by_line(rows, 103) == [(3, D(2020, 2, 1), D(2020, 2, 5), "P", "high")]


def test_superseded_version_stops_at_valid_until():
    # v1 claims a link to 2020-06-30 but is superseded at 2020-03-10 22:00 by v2 ending 2020-02-28:
    # the naive (valid_until-blind) rule would keep the line linked to June.
    v1 = link(202, 2, D(2020, 1, 1), D(2020, 6, 30), T(2020, 1, 5, 22), tier="medium",
              basis="reconstructed_medium", until=T(2020, 3, 10, 22))
    v2 = link(202, 2, D(2020, 1, 1), D(2020, 2, 28), T(2020, 3, 10, 22))
    rows, _ = tool.resolve([v1, v2])
    assert by_line(rows, 202) == [(2, D(2020, 1, 5), D(2020, 3, 9), "P", "medium")]


def test_supersession_is_contiguous_without_gap_or_overlap():
    v1 = link(202, 2, D(2020, 1, 1), D(2020, 6, 30), T(2020, 1, 5, 22), tier="medium",
              basis="reconstructed_medium", until=T(2020, 3, 10, 22))
    v2 = link(202, 2, D(2020, 1, 1), D(2020, 6, 30), T(2020, 3, 10, 22))
    rows, _ = tool.resolve([v1, v2])
    assert by_line(rows, 202) == [(2, D(2020, 1, 5), D(2020, 3, 9), "P", "medium"),
                                  (2, D(2020, 3, 10), D(2020, 6, 30), "P", "high")]
    # an evidence version ending after the mark keeps that day
    v1b = dict(v1, valid_until=T(2020, 3, 10, 22, 30))
    v2b = dict(v2, available_at=T(2020, 3, 10, 22, 30))
    rows, _ = tool.resolve([v1b, v2b])
    assert by_line(rows, 202) == [(2, D(2020, 1, 5), D(2020, 3, 10), "P", "medium"),
                                  (2, D(2020, 3, 11), D(2020, 6, 30), "P", "high")]


def test_ambiguous_line_is_unlinked_and_counted():
    rows, stats = tool.resolve([
        link(303, 3, D(2020, 1, 1), D(2020, 1, 31), T(2019, 12, 1)),
        link(303, 4, D(2020, 1, 11), D(2020, 1, 20), T(2019, 12, 1), primary="N", tier=None,
             basis="current_ticker_verified", status="unknown")])
    assert by_line(rows, 303) == [(3, D(2020, 1, 1), D(2020, 1, 10), "P", "high"),
                                  (3, D(2020, 1, 21), D(2020, 1, 31), "P", "high")]
    assert stats["ambiguous_line_days"] == 10


def test_low_tier_cannot_resurrect_and_is_excluded():
    rows, _ = tool.resolve([
        link(404, 4, D(2020, 1, 1), D(2020, 12, 31), T(2019, 12, 1), until=T(2020, 5, 1, 22)),
        link(404, 4, D(2020, 1, 1), D(2020, 12, 31), T(2020, 5, 1, 22), primary="N", tier="low",
             basis="reconstructed_low")])
    assert by_line(rows, 404) == [(4, D(2020, 1, 1), D(2020, 4, 30), "P", "high")]


def test_n_rows_and_current_ticker_verified_are_excluded():
    rows, _ = tool.resolve([
        link(505, 5, D(2020, 1, 1), D(2020, 1, 31), T(2019, 12, 1), primary="N"),
        link(506, 6, D(2020, 1, 1), None, T(2020, 1, 1), tier=None, basis="current_ticker_verified")])
    assert rows == []


def test_primary_rederived_per_company_by_smallest_line():
    # stored roles are clocked snapshots; at each cutoff P = min visible P/J line of the company
    rows, _ = tool.resolve([
        link(602, 60, D(2020, 1, 1), D(2020, 3, 31), T(2019, 12, 1), primary="P"),
        link(601, 60, D(2020, 2, 1), D(2020, 2, 29), T(2019, 12, 1), primary="J"),
        link(600, 60, D(2020, 1, 1), D(2020, 3, 31), T(2019, 12, 1), primary="N")])
    assert by_line(rows, 601) == [(60, D(2020, 2, 1), D(2020, 2, 29), "P", "high")]
    assert by_line(rows, 602) == [(60, D(2020, 1, 1), D(2020, 1, 31), "P", "high"),
                                  (60, D(2020, 2, 1), D(2020, 2, 29), "J", "high"),
                                  (60, D(2020, 3, 1), D(2020, 3, 31), "P", "high")]
    assert by_line(rows, 600) == []


def test_current_ticker_line_does_not_demote_a_published_primary():
    rows, _ = tool.resolve([
        link(702, 70, D(2020, 1, 1), D(2020, 1, 31), T(2019, 12, 1)),
        link(701, 70, D(2020, 1, 10), None, T(2020, 1, 1), tier=None, basis="current_ticker_verified")])
    assert by_line(rows, 702) == [(70, D(2020, 1, 1), D(2020, 1, 31), "P", "high")]


def test_seal_ignores_later_versions_and_clips_intervals():
    rows, stats = tool.resolve([
        link(801, 8, D(2024, 12, 1), D(2025, 3, 1), T(2024, 11, 30)),
        link(802, 9, D(2024, 1, 1), D(2025, 3, 1), T(2025, 1, 1))])
    assert by_line(rows, 801) == [(8, D(2024, 12, 1), D(2024, 12, 31), "P", "high")]
    assert by_line(rows, 802) == []
    assert stats["versions_visible_before_seal"] == 1
    rows, _ = tool.resolve([link(801, 8, D(2023, 12, 1), D(2025, 3, 1), T(2023, 11, 30))], seal=D(2024, 1, 1))
    assert by_line(rows, 801) == [(8, D(2023, 12, 1), D(2023, 12, 31), "P", "high")]


def test_duplicate_versions_of_one_line_keep_newest_evidence():
    rows, stats = tool.resolve([
        link(901, 90, D(2020, 1, 1), D(2020, 1, 31), T(2019, 12, 1), tier="medium", basis="reconstructed_medium"),
        link(901, 90, D(2020, 1, 1), D(2020, 1, 31), T(2019, 12, 5))])
    assert by_line(rows, 901) == [(90, D(2020, 1, 1), D(2020, 1, 31), "P", "high")]
    assert rows[0]["available_at"] == T(2019, 12, 5)
    assert stats["duplicate_version_line_days"] == 31


def test_line_moving_between_companies_and_invariants_reject_bad_rows():
    rows, _ = tool.resolve([link(111, 11, D(2020, 1, 1), D(2020, 1, 15), T(2019, 12, 1)),
                            link(111, 12, D(2020, 1, 16), D(2020, 1, 31), T(2019, 12, 1))])
    assert by_line(rows, 111) == [(11, D(2020, 1, 1), D(2020, 1, 15), "P", "high"),
                                  (12, D(2020, 1, 16), D(2020, 1, 31), "P", "high")]
    good = dict(sr_id=1, cik=1, start=D(2020, 1, 1), end_incl=D(2020, 1, 5), available_at=T(2019, 1, 1),
                primary="P", tier="high", basis="reconstructed_high", class_status="common")
    with pytest.raises(AssertionError, match="overlapping"):
        tool.verify_invariants([good, dict(good, start=D(2020, 1, 5), end_incl=D(2020, 1, 9))], tool.SEAL)
    with pytest.raises(AssertionError, match="two primary"):
        tool.verify_invariants([good, dict(good, sr_id=2)], tool.SEAL)
    with pytest.raises(AssertionError, match="without a P line"):
        tool.verify_invariants([dict(good, sr_id=2, primary="J")], tool.SEAL)
    with pytest.raises(AssertionError, match="evidence clock"):
        tool.verify_invariants([dict(good, available_at=T(2020, 1, 1, 22, 0, 1))], tool.SEAL)
    tool.verify_invariants([good, dict(good, sr_id=2, primary="J", start=D(2020, 1, 2))], tool.SEAL)


# ---------------------------------------------------------------------------
# Build / publish / pinning
# ---------------------------------------------------------------------------

LINKS = [link(101, 1, D(2020, 1, 2), D(2020, 12, 31), T(2020, 1, 3, 22)),
         link(102, 1, D(2020, 6, 1), D(2021, 6, 30), T(2020, 5, 1), primary="J"),
         link(202, 2, D(2019, 1, 1), D(2021, 12, 31), T(2019, 1, 1), tier="medium", basis="reconstructed_medium"),
         link(303, 3, D(2019, 1, 1), D(2022, 12, 31), T(2019, 1, 1), primary="N"),
         link(404, 4, D(2026, 9, 20), None, T(2026, 9, 20), tier=None, basis="current_ticker_verified")]


def test_build_publishes_pinned_deterministic_output(tmp_path):
    src = make_source(tmp_path, LINKS)
    src_sha = hashlib.sha256((src / "manifest.json").read_bytes()).hexdigest()
    m = tool.build(src, tmp_path / "out", expect_sha256=src_sha)
    out = tmp_path / "out"
    assert m["rehearsal_identity"] is True and m["schema"] == tool.SCHEMA and m["rule"] == tool.RULE
    assert m["source"]["manifest_sha256"] == src_sha
    assert (out / "source/manifest.json").read_bytes() == (src / "manifest.json").read_bytes()
    for name in ("security_company_links", "security_permanent_ids"):
        pinned = (out / f"source/{name}.parquet").read_bytes()
        assert pinned == (src / f"phases/export-001/{name}.part-0000.parquet").read_bytes()
        assert m["pinned_files"][f"{name}.parquet"]["sha256"] == hashlib.sha256(pinned).hexdigest()
    for name, entry in m["files"].items():
        assert hashlib.sha256((out / name).read_bytes()).hexdigest() == entry["sha256"]
    assert (out / "ciks.txt").read_text() == "0000000001\n0000000002\n"
    table, loaded = tool.load_bridge(out)
    assert table.schema.equals(tool.OUT_SCHEMA) and loaded["counts"]["rows"] == table.num_rows
    rows = table.to_pylist()
    assert by_line(rows, 101) == [(1, D(2020, 1, 3), D(2020, 12, 31), "P", "high")]
    assert by_line(rows, 102) == [(1, D(2020, 6, 1), D(2020, 12, 31), "J", "high"),
                                  (1, D(2021, 1, 1), D(2021, 6, 30), "P", "high")]
    assert {r["sr_id"] for r in rows} == {101, 102, 202}
    assert m["counts"]["rows_available_at_exactly_start_mark"] == 1
    assert not list(out.glob(".*.pending"))
    # byte-identical rerun on the same source bytes
    tool.build(src, tmp_path / "out2")
    for f in ("links.parquet", "ciks.txt", "manifest.json", "source/manifest.json"):
        assert (out / f).read_bytes() == (tmp_path / "out2" / f).read_bytes()
    # the source is untouched
    assert sorted(p.name for p in src.rglob("*")) == ["export-001", "manifest.json", "phases",
                                                     "security_company_links.part-0000.parquet",
                                                     "security_permanent_ids.part-0000.parquet"]


def test_build_refuses_bad_source_or_output(tmp_path):
    src = make_source(tmp_path, LINKS)
    with pytest.raises(ValueError, match="expect-source-sha256"):
        tool.build(src, tmp_path / "o1", expect_sha256="0" * 64)
    with pytest.raises(ValueError, match="inside"):
        tool.build(src, src / "bridge")
    (tmp_path / "busy").mkdir()
    (tmp_path / "busy" / "x").write_text("x")
    with pytest.raises(FileExistsError):
        tool.build(src, tmp_path / "busy")
    blob = src / "phases/export-001/security_company_links.part-0000.parquet"
    blob.write_bytes(blob.read_bytes()[:-1] + b"\0")
    with pytest.raises(ValueError, match="do not match the source manifest"):
        tool.build(src, tmp_path / "o2")
    assert not (tmp_path / "o2").exists()


def test_build_refuses_lines_outside_vendor_namespace(tmp_path):
    src = make_source(tmp_path, LINKS)
    perm = src / "phases/export-001/security_permanent_ids.part-0000.parquet"
    rows = pq.read_table(perm).to_pylist()
    rows[0]["security_id"] = "OTHER-1"
    blob = parquet_bytes(rows, PERM_SCHEMA)
    perm.write_bytes(blob)
    manifest = json.loads((src / "manifest.json").read_text())
    for entry in manifest["files"]:
        if entry["dataset"] == "security_permanent_ids":
            entry.update(bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest())
    (src / "manifest.json").write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match="namespace"):
        tool.build(src, tmp_path / "out")


def test_load_bridge_detects_tampering(tmp_path):
    out = tmp_path / "out"
    tool.build(make_source(tmp_path, LINKS), out)
    (out / "links.parquet").write_bytes((out / "links.parquet").read_bytes() + b"\0")
    with pytest.raises(ValueError, match="do not match"):
        tool.load_bridge(out)


# ---------------------------------------------------------------------------
# --check (metadata only)
# ---------------------------------------------------------------------------

def make_role(root: Path, name, ids, days, member, score_begin, score_end):
    d = root / name
    d.mkdir()
    sessions = np.array([(x - D(1970, 1, 1)).days * tool.DAY_NS for x in days], dtype="<i8").tobytes()
    blobs = {"sessions.i64": sessions, "ids.u64": np.array(ids, dtype="<u8").tobytes(),
             "member.u8": np.asarray(member, dtype="u1").tobytes()}
    files = {}
    for fname, blob in blobs.items():
        (d / fname).write_bytes(blob)
        files[fname] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    (d / "manifest.json").write_text(json.dumps({
        "schema": tool.ROLE_SCHEMA, "status": "complete", "instrument_namespace": "spiderrock.securityID",
        "dates": len(days), "instruments": len(ids), "files": files,
        "score_begin": score_begin, "score_end": score_end}))
    return d


def test_check_prints_per_year_role_coverage(tmp_path):
    out = tmp_path / "out"
    tool.build(make_source(tmp_path, LINKS), out)
    days = [D(2019, 12, 30), D(2019, 12, 31), D(2020, 1, 2), D(2020, 1, 3), D(2020, 6, 1), D(2021, 1, 4)]
    ids = [101, 102, 202, 999]
    member = np.ones((len(days), len(ids)), dtype="u1")
    member[3, 3] = 0
    role = make_role(tmp_path, "role", ids, days, member, score_begin=2, score_end=6)
    buf = io.StringIO()
    recs = tool.check(out, [role], out=buf)
    printed = [json.loads(line) for line in buf.getvalue().splitlines()]
    assert printed == json.loads(json.dumps(recs))
    y = {r["year"]: r for r in recs[1:]}
    assert set(y) == {2020, 2021, "scored"}
    # 2020 scored sessions: 01-02, 01-03, 06-01; member cells 3*4 - 1 = 11
    # P-linked: 101 on 01-03 and 06-01; 202 on all three -> 5 cells; 102 is J on 06-01 (P-or-J 6)
    assert y[2020]["member_cells"] == 11 and y[2020]["member_cells_p_linked"] == 5
    assert y[2020]["cell_coverage_p_or_j"] == round(6 / 11, 4)
    assert y[2020]["role_ids_member"] == 4 and y[2020]["role_ids_member_p_linked"] == 2
    # 2021-01-04: 102 is now P, 202 P, 101 ended -> 2 of 4
    assert y[2021]["member_cells_p_linked"] == 2 and y[2021]["cell_coverage_p"] == 0.5
    assert y["scored"]["member_cells"] == 15 and y["scored"]["member_cells_p_linked"] == 7


def test_check_static_bridge_diagnostic(tmp_path):
    duckdb = pytest.importorskip("duckdb")
    out = tmp_path / "out"
    tool.build(make_source(tmp_path, LINKS), out)
    days = [D(2020, 6, 1), D(2021, 1, 4)]
    ids = [101, 102, 202, 303, 999]
    role = make_role(tmp_path, "role", ids, days, np.ones((2, 5), dtype="u1"), 0, 2)
    wh = tmp_path / "wh.duckdb"
    con = duckdb.connect(str(wh))
    con.execute("create table security_identifier_history (id_type varchar, id_value varchar, security_id varchar)")
    con.executemany("insert into security_identifier_history values (?, ?, ?)", [
        ("TBLTICKERHISTORY_SECURITY_ID", "101", "SEC-CIK-0000000001"),   # agree
        ("TBLTICKERHISTORY_SECURITY_ID", "202", "SEC-CIK-0000000007"),   # disagree
        ("TBLTICKERHISTORY_SECURITY_ID", "303", "SEC-CIK-0000000003"),   # static only
        ("OTHER", "999", "SEC-CIK-0000000009")])
    con.close()
    recs = tool.check(out, [role], static_warehouse=wh, out=io.StringIO())
    diag = recs[-1]
    assert (diag["agree"], diag["disagree"], diag["bridge_only"], diag["static_only"]) == (1, 1, 1, 1)
    recs = tool.check(out, [role], static_warehouse=tmp_path / "missing.duckdb", out=io.StringIO())
    assert recs[-1]["static_bridge"] == "unavailable"


def test_cli_build_and_check(tmp_path, capsys):
    src = make_source(tmp_path, LINKS)
    assert tool.main(["--output", str(tmp_path / "out"), "--source", str(src)]) == 0
    assert json.loads(capsys.readouterr().out)["counts"]["ciks"] == 2
    role = make_role(tmp_path, "role", [101], [D(2020, 6, 1)], np.ones((1, 1), dtype="u1"), 0, 1)
    assert tool.main(["--check", str(tmp_path / "out"), "--role", str(role)]) == 0
    lines = [json.loads(x) for x in capsys.readouterr().out.splitlines()]
    assert lines[-1]["year"] == "scored" and lines[-1]["cell_coverage_p"] == 1.0
    with pytest.raises(SystemExit):
        tool.main(["--output", str(tmp_path / "o3"), "--source", str(src), "--seal", "2025-06-01"])
