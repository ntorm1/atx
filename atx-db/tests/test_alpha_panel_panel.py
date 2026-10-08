"""Panel v3 bindings (toy lake): fundamentals v10 visibility clock, point-in-time link tiers, appended columns and
the output-stage option of ``panel assemble``."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path

import duckdb
import pytest

from atx_db.alpha_panel import panel as P

SESSIONS = [dt.date(2022, 2, 28), dt.date(2022, 3, 1), dt.date(2022, 3, 2), dt.date(2022, 3, 3), dt.date(2022, 3, 4)]
TS = dt.datetime
# security_id -> (cik, tier, basis, available_at); line 5 has no link
LINKS = {
    1: (100, "strict", "reconstructed_high", TS(2021, 12, 1)),
    2: (200, "backfill", "snapshot_run_backfill", TS(2021, 12, 1)),        # snapshot tier: never joined
    3: (300, "dated", "sec_filing_symbol", TS(2021, 12, 1)),
    4: (400, "name", "finra_name_match", TS(2022, 3, 2, 22, 0, 0)),        # known exactly at 03-02's mark
}
LINES = (1, 2, 3, 4, 5)


def _copy(con: duckdb.DuckDBPyConnection, sql: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    con.execute(f"COPY ({sql}) TO '{dest.as_posix()}' (FORMAT PARQUET)")


def _event(cik: int, acc: str, clock: TS, avail: TS | None, be: float, fx: bool | None) -> str:
    """One events row; ``avail`` None writes a v9-style row (the stage then has no available_at / fx columns)."""
    v10 = "" if avail is None else f", TIMESTAMP '{avail}' AS available_at, {fx} AS fx_converted, 1.0 AS fx_rate, " \
                                   "false AS is_restated"
    return (f"SELECT {cik}::BIGINT AS cik, '{acc}' AS accession, '10-Q' AS form, DATE '{clock.date()}' AS filed, "
            f"TIMESTAMP '{clock}' AS clock_utc, 'fsds_accepted_utc' AS clock_basis, DATE '2021-12-31' AS period_end, "
            "2021 AS fiscal_year, 'Q4' AS fiscal_period, 1000.0 AS at, "
            f"{be} AS be, {be} * 3 AS sale_ttm, {be} / 2 AS ni_ttm, 'USD' AS currency, 0.5 AS sue{v10}")


# cik 100: E2 clock_utc is early but available_at 21:59:59 on 03-01 (visible 03-02); E3 clock_utc is early but
# available_at is exactly 03-02 22:00 (not visible on 03-03, whose cutoff is 03-02 22:00; visible 03-04)
EVENTS = [
    (100, "a1", TS(2022, 2, 1, 12), TS(2022, 2, 1, 12), 10.0, False),
    (100, "a2", TS(2022, 3, 1, 10), TS(2022, 3, 1, 21, 59, 59), 20.0, False),
    (100, "a3", TS(2022, 3, 1, 11), TS(2022, 3, 2, 22, 0, 0), 30.0, True),
    (200, "b1", TS(2022, 1, 5, 12), TS(2022, 1, 5, 12), 99.0, False),
    (300, "c1", TS(2022, 1, 5, 12), TS(2022, 1, 5, 12), 5.0, False),
    (400, "d1", TS(2022, 1, 5, 12), TS(2022, 1, 5, 12), 7.0, False),
]


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    for env in (P.OUT_STAGE_ENV, P.FUND_STAGE_ENV, P.LINK_TABLE_ENV):
        monkeypatch.delenv(env, raising=False)
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    con = duckdb.connect()
    cal = "SELECT unnest([" + ", ".join(f"DATE '{d}'" for d in SESSIONS) + "]) AS session_date"
    _copy(con, cal, tmp_path / "calendar.parquet")
    ivs = ", ".join(f"CAST(NULL AS DOUBLE) AS iv_atm_{h}d" for h in (5, 10, 21, 42, 63, 126, 252))
    _copy(con, f"""
        SELECT c.session_date, s.security_id,
               row_number() OVER (PARTITION BY s.security_id ORDER BY c.session_date) AS sidx,
               'T' || s.security_id AS ticker, 10.0 AS open, 11.0 AS high, 9.0 AS low, 10.0 AS raw_close,
               10.0 AS close, 0.0 AS ret, false AS ret_guarded, 1e6 AS volume, 1e7 AS dollar_volume,
               10.0 AS raw_close_prev, true AS present_prev, 1e7 AS adv63_eligible, 1e7 AS adv63,
               1e7 AS shares_out_pre_si, DATE '2021-11-01' AS shares_obs_date, false AS shares_withheld,
               CAST(NULL AS DECIMAL(2,1)) AS earn_recent, CAST(NULL AS VARCHAR) AS earn_flag,
               CAST(NULL AS VARCHAR) AS gics, false AS is_operating, false AS is_index, {ivs}
        FROM ({cal}) c CROSS JOIN (SELECT unnest({list(LINES)})::BIGINT AS security_id) s""",
          tmp_path / "_tmp" / "panel_core" / "bucket_00.parquet")
    _copy(con, f"""
        SELECT c.session_date, s.security_id, true AS member, s.security_id AS adv_rank, 0.0 AS mkt_ret, 5 AS mkt_n
        FROM ({cal}) c CROSS JOIN (SELECT unnest({list(LINES)})::BIGINT AS security_id) s""",
          tmp_path / "_tmp" / "panel_member" / "year=2022.parquet")
    links = " UNION ALL ".join(
        f"SELECT {sid}::BIGINT AS security_id, {cik}::BIGINT AS cik, DATE '2018-01-02' AS valid_from, "
        f"DATE '2026-09-18' AS valid_to, 100.0 AS sessions, '{tier}' AS link_tier, '{basis}' AS basis, "
        f"true AS is_issuer_primary, CAST(NULL AS VARCHAR) AS share_class, 1 AS max_issuer_lines, "
        f"TIMESTAMP '{avail}' AS available_at, TIMESTAMP '{avail}' AS evidence_at"
        for sid, (cik, tier, basis, avail) in LINKS.items())
    _copy(con, links, tmp_path / "identity" / "link_table_v3.parquet")
    (tmp_path / "identity" / "link_table_v3_manifest.json").write_text("{}")
    _copy(con, " UNION ALL ".join(_event(*e) for e in EVENTS), tmp_path / "fundamentals_v10" / "events.parquet")
    (tmp_path / "fundamentals_v10" / "manifest.json").write_text("{}")
    # v9-style stage: same events, no available_at / fx columns
    _copy(con, " UNION ALL ".join(_event(c, a, k, None, be, None) for c, a, k, _v, be, _f in EVENTS),
          tmp_path / "fundamentals" / "events.parquet")
    (tmp_path / "fundamentals" / "manifest.json").write_text("{}")
    yield tmp_path, con
    con.close()


def _assemble(root: Path, con: duckdb.DuckDBPyConnection, bind: P.Bindings) -> dict[tuple, dict]:
    mcon = P.C.connect(memory="200MB", threads=1, db_file="t_panel.duckdb")
    try:
        P.assemble_month(mcon, 2022, 3, bind)
    finally:
        mcon.close()
    out = root / bind.out_stage / "year=2022" / "panel-03.parquet"
    rows = con.execute(f"SELECT * FROM read_parquet('{out.as_posix()}', hive_partitioning = false)").fetchall()
    cols = [d[0] for d in con.description]
    return {(r[0], r[1]): dict(zip(cols, r, strict=True)) for r in rows}


def _d(day: int) -> dt.date:
    return dt.date(2022, 3, day)


def test_defaults_bind_v10_and_link_table_v3(lake):
    b = P.bindings()
    assert (b.out_stage, b.fund_stage, b.link_table) == ("panel", "fundamentals_v10", "identity/link_table_v3.parquet")
    assert b.link_manifest() == "identity/link_table_v3_manifest.json"
    assert "fundamentals_v10/manifest.json" in b.input_manifests()
    assert "fundamentals/manifest.json" not in b.input_manifests()


def test_bindings_flag_over_env_over_default(monkeypatch):
    monkeypatch.setenv(P.OUT_STAGE_ENV, "panel_env")
    monkeypatch.setenv(P.FUND_STAGE_ENV, "fundamentals")
    monkeypatch.delenv(P.LINK_TABLE_ENV, raising=False)
    b = P.bindings()
    assert (b.out_stage, b.fund_stage, b.link_table) == ("panel_env", "fundamentals", P.DEFAULT_LINK_TABLE)
    assert P.bindings(out_stage="panel_v3").out_stage == "panel_v3"
    for bad in ("fundamentals_v10", "panel/../prices", "../panel"):
        with pytest.raises(ValueError):
            P.bindings(out_stage=bad)


def test_v10_available_at_clock_and_22utc_boundary(lake):
    root, con = lake
    rows = _assemble(root, con, P.bindings(out_stage="panel_v3"))
    be = {d.day: rows[(d, 1)]["be"] for d in SESSIONS[1:]}
    # 03-01: cutoff 02-28 22:00 -> a1 only. 03-02: cutoff 03-01 22:00 > a2 (21:59:59). 03-03: cutoff 03-02 22:00 is
    # not > a3's available_at (exactly 22:00) -> still a2. 03-04: a3.
    assert be == {1: 10.0, 2: 20.0, 3: 20.0, 4: 30.0}
    assert rows[(_d(3), 1)]["fund_clock"] == TS(2022, 3, 1, 21, 59, 59)   # fund_clock is the visibility clock
    assert rows[(_d(4), 1)]["fund_clock"] == TS(2022, 3, 2, 22, 0, 0)
    assert rows[(_d(4), 1)]["sale_ttm"] == 90.0 and rows[(_d(4), 1)]["ni_ttm"] == 15.0
    assert rows[(_d(3), 1)]["fx_converted"] is False and rows[(_d(4), 1)]["fx_converted"] is True


def test_v9_stage_falls_back_to_clock_utc(lake):
    root, con = lake
    rows = _assemble(root, con, P.bindings(out_stage="panel_v9", fund_stage="fundamentals"))
    # clock_utc of a2 / a3 (03-01 10:00 / 11:00) is before 03-01 22:00: a3 is visible from 03-02
    assert {d.day: rows[(d, 1)]["be"] for d in SESSIONS[1:]} == {1: 10.0, 2: 30.0, 3: 30.0, 4: 30.0}
    assert rows[(_d(2), 1)]["fx_converted"] is None


def test_backfill_tier_never_joined_and_tier_carried(lake):
    root, con = lake
    rows = _assemble(root, con, P.bindings(out_stage="panel_v3"))
    for d in SESSIONS[1:]:
        r2 = rows[(d, 2)]  # backfill-only line: unlinked, no issuer fields
        assert (r2["cik"], r2["link_tier"], r2["link_basis"], r2["be"], r2["fund_clock"]) == (None, None, None, None, None)
        assert r2["is_issuer_primary"] is None and r2["me_company"] is None
        r1, r3 = rows[(d, 1)], rows[(d, 3)]
        assert (r1["cik"], r1["link_tier"], r1["link_basis"]) == (100, "strict", "reconstructed_high")
        assert (r3["cik"], r3["link_tier"], r3["link_basis"], r3["be"]) == (300, "dated", "sec_filing_symbol", 5.0)
        assert rows[(d, 5)]["cik"] is None
    assert not any(r["link_tier"] == "backfill" for r in rows.values())


def test_link_used_once_known_at_session_mark(lake):
    root, con = lake
    rows = _assemble(root, con, P.bindings(out_stage="panel_v3"))
    # line 4's name link is known at 2022-03-02 22:00:00 = 03-02's mark (<=): unlinked on 03-01, linked from 03-02
    assert rows[(_d(1), 4)]["cik"] is None and rows[(_d(1), 4)]["be"] is None
    assert (rows[(_d(2), 4)]["cik"], rows[(_d(2), 4)]["link_tier"], rows[(_d(2), 4)]["be"]) == (400, "name", 7.0)


def test_layout_appends_only_and_drops_event_metadata(lake):
    root, con = lake
    _assemble(root, con, P.bindings(out_stage="panel_v3"))
    _assemble(root, con, P.bindings(out_stage="panel_v9", fund_stage="fundamentals"))
    s = {}
    for stage in ("panel_v3", "panel_v9"):
        f = (root / stage / "year=2022" / "panel-03.parquet").as_posix()
        s[stage] = [(c[0], c[1]) for c in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{f}', hive_partitioning = false)").fetchall()]
    assert s["panel_v3"] == s["panel_v9"]  # v10 metadata columns never leak into the item list
    names = [c for c, _t in s["panel_v3"]]
    assert names[-4:] == ["me_company", "is_issuer_primary", "fx_converted", "link_basis"]
    assert dict(s["panel_v3"])["fx_converted"] == "BOOLEAN" and dict(s["panel_v3"])["link_basis"] == "VARCHAR"
    assert not {"available_at", "fx_rate", "is_restated"} & set(names)
    i = names.index("fund_clock_basis")
    assert names[i + 1:i + 6] == ["at", "be", "sale_ttm", "ni_ttm", "currency"] and names[i + 6] == "sue"


def _tree(path: Path) -> dict[str, str]:
    return {p.relative_to(path).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(path.rglob("*")) if p.is_file()}


def test_out_stage_writes_only_the_chosen_stage(lake):
    root, _con = lake
    panel = root / "panel"
    (panel / "year=2022").mkdir(parents=True)
    (panel / "year=2022" / "panel-03.parquet").write_bytes(b"v2 bytes")
    (panel / "manifest.json").write_text('{"schema": "v2"}')
    before = _tree(panel)
    assert P.main(["--stages", "assemble", "--years", "2022", "--out-stage", "panel_v3"]) == 0
    assert _tree(panel) == before
    out = root / "panel_v3"
    assert sorted(_tree(out)) == ["manifest.json", "year=2022/panel-02.parquet", "year=2022/panel-03.parquet"]
    m = json.loads((out / "manifest.json").read_text())
    assert m["stage"] == "panel_v3" and m["status"] == "complete"
    assert m["bindings"]["fund_stage"] == "fundamentals_v10" and m["bindings"]["link_tiers"] == ["strict", "dated", "name"]
    assert set(m["input_manifests_sha256"]) == {"identity/link_table_v3_manifest.json", "fundamentals_v10/manifest.json"}
    assert m["assemble"]["2022"]["rows"] == 5 * len(LINES)
    assert not list((root / "_tmp").glob("panel_v3_*.duckdb*"))  # month scratch removed


def test_out_stage_refuses_spine_stages(lake):
    with pytest.raises(ValueError, match="shared spine"):
        P.run(["membership"], [2022], P.bindings(out_stage="panel_v3"))
